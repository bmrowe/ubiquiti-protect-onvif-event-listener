#!/usr/bin/env bash
# Run /tmp/enrich_events on the router for every distinct (cameraId, date)
# pair that has events.  Uses per-camera image dimensions from cameras.channels.
# Idempotent: enrich_events uses ON CONFLICT DO NOTHING for smartDetectObjects
# / smartDetectObjectAreas, and rewrites events.metadata each call.
#
# Run from the dev host: `bash tools/enrich_all_router.sh`
# Assumes /tmp/enrich_events is already deployed to root@192.168.1.1.

set -euo pipefail

HOST=${HOST:-root@192.168.1.1}

# Build the (cameraId, date, w, h) inventory on the router and stream back.
PAIRS=$(ssh "$HOST" "psql -h /run/postgresql -p 5433 -U postgres unifi-protect -tAc \"
  SELECT e.\\\"cameraId\\\" || ' ' ||
         to_char(to_timestamp(e.start/1000) AT TIME ZONE 'UTC', 'YYYY-MM-DD') || ' ' ||
         COALESCE((c.channels::json->0->>'width')::int,  2560) || ' ' ||
         COALESCE((c.channels::json->0->>'height')::int, 1440)
  FROM events e
  LEFT JOIN cameras c ON c.id = e.\\\"cameraId\\\"
  WHERE e.\\\"cameraId\\\" IS NOT NULL
  GROUP BY e.\\\"cameraId\\\",
           to_char(to_timestamp(e.start/1000) AT TIME ZONE 'UTC', 'YYYY-MM-DD'),
           (c.channels::json->0->>'width')::int,
           (c.channels::json->0->>'height')::int
  ORDER BY 1;\"")

total=$(echo "$PAIRS" | wc -l)
echo "running enrich_events for $total (camera, date) pairs"
n=0
while IFS=' ' read -r cam day w h; do
  [ -z "$cam" ] && continue
  n=$((n+1))
  printf "[%3d/%3d] cam=%s day=%s %sx%s  " "$n" "$total" "$cam" "$day" "$w" "$h"
  ssh -n "$HOST" "/tmp/enrich_events --camera_id='$cam' --date='$day' \
                                      --image_width=$w --image_height=$h" 2>&1
done <<<"$PAIRS"
