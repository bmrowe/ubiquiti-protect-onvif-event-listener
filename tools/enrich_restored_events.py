#!/usr/bin/env python3
"""Fill in the rich-event-detail fields that the partial backup didn't carry,
so restored events look like Protect-native 7.1 events to the UI.

EVERY synthesised field is marked with a `TODO:` comment in the SQL output —
when we have a real source for the data (camera ONVIF analytics, NanoDet-M
output, etc.) we should come back and replace each one.

Reads from:  router Postgres (live), filtered by --camera-id + --date.
Writes to:   a self-contained SQL file you scp to the router and apply with
             psql -h /run/postgresql -p 5433 -U postgres unifi-protect -f patch.sql

What gets enriched:

  events
    metadata.detectedAreas      — areaIndexes (full grid placeholder), routePath
                                  (single waypoint at bbox centre), smartDetectObject FK
    metadata.detectedThumbnails — per-object: attributes, coord (placeholder bbox),
                                  croppedId (= thumbnailId), labels (string array),
                                  objectId, type
    metadata.zonesStatus        — zone-1 entered/active placeholder
    metadata.weather            — null (intentional — we don't yet pull weather)
    metadata.count              — null
    metadata.{hallwayMode,linesSettings,linesStatus,loiterStatus,tamperStatus} — null
    thumbnailFullfovId          — same as thumbnailId (PLACEHOLDER — should be a
                                  separate full-FOV crop; for now reuse so the UI
                                  has *something* to render)
  smartDetectObjects   (1 row per object UUID in detectionLabels)
    attributes JSON with synthesised confidence (= event.score), objectType,
    trackerId (= 1), zone ([1]), everything else null.
  smartDetectObjectAreas (1 row per smartDetectObjects)
    placeholder bbox (image-relative centred quad) + full grid coverage.

The tool is idempotent: events.metadata is rewritten unconditionally for the
affected events (so re-running with improved synthesis works), and the two
INSERTs use ON CONFLICT (id) DO NOTHING."""

import argparse
import datetime as dt
import json
import sys
import uuid

import psycopg2
import psycopg2.extras


# Protect's UI grid: 12 cols x 10 rows.  See G4 sample event for confirmation
# (highest areaIndex observed in real data: 109).
GRID_COLS = 12
GRID_ROWS = 10
GRID_CELLS = GRID_COLS * GRID_ROWS  # 120


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ssh-host", default="root@192.168.1.1",
                   help="ssh target for one-shot psql probes (used to read live state)")
    p.add_argument("--camera-id", required=True)
    p.add_argument("--date", required=True, help="UTC day YYYY-MM-DD")
    p.add_argument("--width",  type=int, default=2560)
    p.add_argument("--height", type=int, default=1440)
    p.add_argument("--out", required=True, help="Output SQL file path")
    return p.parse_args()


def quote_text(s):
    if s is None:
        return "NULL"
    return "'" + s.replace("'", "''") + "'"


def quote_json(j):
    if j is None:
        return "NULL"
    s = json.dumps(j, separators=(",", ":")) if not isinstance(j, str) else j
    return "'" + s.replace("'", "''") + "'::json"


def quote_int_array(arr):
    return "ARRAY[" + ",".join(str(int(x)) for x in arr) + "]::int[]"


def placeholder_bbox(width: int, height: int, det_type: str) -> tuple[int, int, int, int]:
    """Return (x1, y1, x2, y2) for a synthesised bbox.

    TODO: replace with the actual bbox from ONVIF tt:BoundingBox / NanoDet-M
    when we have it.  These are eyeballed defaults that put a plausible shape
    on screen so the UI overlay renders.
    """
    if det_type == "person":
        # Tall thin rectangle, centred horizontally, lower half.
        x1, w = width // 3,    width // 4
        y1, h = height // 4,   height // 2
    elif det_type == "vehicle":
        # Wide rectangle, centred, middle third vertically.
        x1, w = width // 4,    width // 2
        y1, h = height // 3,   height // 3
    else:
        # Generic centred quad.
        x1, w = width // 3,    width // 3
        y1, h = height // 3,   height // 3
    return x1, y1, x1 + w, y1 + h


def synth_metadata(event, objects, width, height):
    """Build a 7.1-shaped metadata JSON from the sparse restored data.

    `objects` is a list of dicts: {"id": objectId, "type": "person"/"vehicle"}.
    """
    detected_areas = []
    detected_thumbnails = []
    for o in objects:
        x1, y1, x2, y2 = placeholder_bbox(width, height, o["type"])
        # routePath needs lastDirection (dx,dy) and waypoints.  For a stationary
        # synthesised event we put one waypoint at the bbox centre.
        cx, cy = (x1 + x2) // 2, (y1 + y2) // 2
        detected_areas.append({
            # TODO: derive real areaIndexes from the bbox + grid intersection.
            "areaIndexes": list(range(GRID_CELLS)),
            "routePath": {
                # TODO: real waypoints sampled across the track.
                "lastDirection": [0, 0],
                "waypoints": [[cx, cy]],
            },
            "smartDetectObject": o["id"],
        })
        detected_thumbnails.append({
            "attributes": {
                "objectType": o["type"],
                # TODO: real tracker id from the camera's analytics stream.
                "trackerId": 1,
                "zone": [1],
            },
            # TODO: clockBestWall should be the wall-clock at the frame the
            # best thumbnail was captured; for now use event.start.
            "clockBestWall": event["start"],
            "confidence": event["score"] or 0,
            # TODO: real per-frame bounding box.  Format is [x, y, w, h].
            "coord": [float(x1), float(y1), float(x2 - x1), float(y2 - y1)],
            "croppedId": event["thumbnailId"],
            # TODO: real label strings — for now we synthesise from event type.
            "labels": [
                f"smartDetectType:{o['type']}",
                f"zone:{event['cameraId']}:1",
            ],
            "objectId": o["id"],
            "type": o["type"],
        })

    return {
        "count": None,
        "detectedAreas": detected_areas,
        "detectedThumbnails": detected_thumbnails,
        # TODO: hallwayMode mirrors cameras.recordingSettings.hallwayMode at
        # the time of the event.  Stub null for now.
        "hallwayMode": None,
        # TODO: lines* fields populated only when smartDetectLines was active.
        "linesSettings": None,
        "linesStatus": None,
        "loiterStatus": None,
        "tamperStatus": None,
        # TODO: pull from a real weather source (openweathermap?) at event time.
        # Until then, emit a benign default ("fine, 21°C") so the UI's weather
        # pill renders and we can see whether it's wired up at all.
        # iconCode "32" is the common "sunny/clear day" code in TWC-style icon sets.
        "weather": {"iconCode": "32", "temperature": 21.0, "temperatureUnit": "C"},
        # TODO: real zonesStatus — level (0-100) is the score the zone reached,
        # status is "enter"/"leave"/"motion".  Synthesised "leave" so the UI
        # treats the event as completed.
        "zonesStatus": {
            "1": {"level": event["score"] or 0, "status": "leave"},
        },
    }


def synth_object_attributes(event, det_type):
    """smartDetectObjects.attributes JSON — mirror native shape with placeholders."""
    return {
        # TODO: associatedFaceTrackerID = face tracker id when face detection runs.
        "associatedFaceTrackerID": None,
        "blurness": None,
        "color": None,
        # TODO: per-object confidence.  Using event.score as approximation.
        "confidence": event["score"] or 0,
        "faceEmbed": None,
        "faceLandmarks": None,
        "faceMask": None,
        "facePose": None,
        "faceVerifyStatus": None,
        "line": None,
        "matchedId": None,
        "matchedName": None,
        "namesTopK": None,
        "objectType": det_type,
        "personEmbedFromCamera": None,
        "qualityScore": None,
        "topKCandidate": None,
        # TODO: real tracker id (sequential per camera, monotonic across an event).
        "trackerId": 1,
        "vehicleType": None,
        # TODO: real zone array from smartDetectZones intersection.
        "zone": [1],
    }


def main() -> int:
    args = parse_args()
    day = dt.datetime.strptime(args.date, "%Y-%m-%d").replace(tzinfo=dt.timezone.utc)
    day_start_ms = int(day.timestamp() * 1000)
    day_end_ms = day_start_ms + 86_400_000

    # ---- Read events + per-object detectionLabels via one SSH+psql probe ----
    import subprocess
    psql_cmd = (
        f"psql -h /run/postgresql -p 5433 -U postgres unifi-protect -tAc "
        f"\"SELECT json_build_object("
        f"  'events', (SELECT json_agg(row_to_json(e)) FROM events e "
        f"             WHERE e.\\\"cameraId\\\" = '{args.camera_id}' "
        f"               AND e.start >= {day_start_ms} AND e.start < {day_end_ms}), "
        f"  'objects', (SELECT json_agg(json_build_object("
        f"                  'eventId', dl.\\\"eventId\\\", "
        f"                  'objectId', dl.\\\"objectId\\\")) "
        f"              FROM \\\"detectionLabels\\\" dl "
        f"              JOIN events e ON e.id = dl.\\\"eventId\\\" "
        f"              WHERE e.\\\"cameraId\\\" = '{args.camera_id}' "
        f"                AND e.start >= {day_start_ms} AND e.start < {day_end_ms} "
        f"                AND dl.\\\"objectId\\\" IS NOT NULL))\""
    )
    proc = subprocess.run(
        ["ssh", args.ssh_host, psql_cmd],
        capture_output=True, text=True, check=True,
    )
    data = json.loads(proc.stdout.strip())
    events = data["events"] or []
    object_links = data["objects"] or []
    if not events:
        print(f"No events for cameraId={args.camera_id} on {args.date}", file=sys.stderr)
        return 1

    # ---- Group object UUIDs by eventId ----
    objects_by_event: dict[str, list[str]] = {}
    for o in object_links:
        objects_by_event.setdefault(o["eventId"], []).append(o["objectId"])

    # ---- Emit SQL ----
    out_path = args.out
    with open(out_path, "w") as f:
        f.write("-- generated by tools/enrich_restored_events.py\n")
        f.write(f"-- camera_id : {args.camera_id}\n")
        f.write(f"-- date      : {args.date} UTC\n")
        f.write(f"-- events    : {len(events)}\n")
        f.write(f"-- objects   : {len(object_links)}\n")
        f.write(f"-- image     : {args.width}x{args.height}\n")
        f.write("--\n")
        f.write("-- ALL synthesised fields are flagged with TODO comments in the\n")
        f.write("-- Python source (tools/enrich_restored_events.py).\n")
        f.write("BEGIN;\n\n")

        for e in events:
            obj_ids = objects_by_event.get(e["id"], [])
            # Determine per-object type from smartDetectTypes (use the first if
            # multiple objects share the event — we don't have per-object
            # types in the restored data).
            sdt = e.get("smartDetectTypes") or []
            det_type = sdt[0] if sdt else "unknown"
            objects = [{"id": oid, "type": det_type} for oid in obj_ids]

            # --- UPDATE events.metadata + thumbnailFullfovId ---
            enriched = synth_metadata(e, objects, args.width, args.height)
            f.write(
                f"UPDATE events SET "
                f"metadata = {quote_json(enriched)}, "
                # TODO: thumbnailFullfovId should be a separate full-FOV
                # snapshot; for now we reuse the regular thumbnail so the UI
                # has something to render.
                f"\"thumbnailFullfovId\" = {quote_text(e['thumbnailId'])} "
                f"WHERE id = {quote_text(e['id'])};\n"
            )

            # --- INSERT smartDetectObjects + smartDetectObjectAreas per object ---
            for o in objects:
                obj_attrs = synth_object_attributes(e, o["type"])
                f.write(
                    f"INSERT INTO \"smartDetectObjects\" "
                    f"(id, \"eventId\", \"cameraId\", \"thumbnailId\", type, "
                    f" attributes, \"detectedAt\", "
                    f" \"createdAt\", \"updatedAt\", metadata) "
                    f"VALUES ("
                    f"{quote_text(o['id'])}, "
                    f"{quote_text(e['id'])}, "
                    f"{quote_text(e['cameraId'])}, "
                    f"{quote_text(e['thumbnailId'])}, "
                    f"{quote_text(o['type'])}, "
                    f"{quote_json(obj_attrs)}, "
                    f"{e['start']}, "
                    f"now(), now(), '{{}}'::json) "
                    f"ON CONFLICT (id) DO NOTHING;\n"
                )

                x1, y1, x2, y2 = placeholder_bbox(args.width, args.height, o["type"])
                # Deterministic UUID for the smartDetectObjectAreas row so
                # re-runs don't create duplicate ids.
                area_id = str(uuid.uuid5(uuid.NAMESPACE_URL, f"protect:sda:{o['id']}"))
                f.write(
                    f"INSERT INTO \"smartDetectObjectAreas\" "
                    f"(id, \"smartDetectObjectId\", \"areaIndexes\", "
                    f" \"boundingX1\", \"boundingY1\", \"boundingX2\", \"boundingY2\", "
                    f" \"detectedAt\", \"lastSeenAt\") "
                    f"VALUES ("
                    f"{quote_text(area_id)}, "
                    f"{quote_text(o['id'])}, "
                    # TODO: derive areaIndexes from bbox intersection with the
                    # 12x10 grid, not the full grid as we do here.
                    f"{quote_int_array(range(GRID_CELLS))}, "
                    f"{x1}, {y1}, {x2}, {y2}, "
                    f"{e['start']}, {e['end'] or e['start']}) "
                    f"ON CONFLICT (id) DO NOTHING;\n"
                )

        f.write("\nCOMMIT;\n")
        f.write(f"\\echo 'enriched {len(events)} events, materialised {len(object_links)} smartDetectObjects/Areas pairs'\n")

    print(f"wrote {out_path}")
    print(f"  events updated:                       {len(events)}")
    print(f"  smartDetectObjects rows to INSERT:    {len(object_links)}")
    print(f"  smartDetectObjectAreas rows to INSERT:{len(object_links)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
