// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

// Auto-recover events from Protect's own daily DB backups when we detect the
// events table has been wiped (typical cause: a firmware upgrade that resets
// the Protect cluster without warning us).  The recordingFiles + thumbnails
// UBV files survive the wipe and still hold the user's history; recovering
// the events row + rich-format sibling rows means the UI re-surfaces those
// recordings with the right markers.
//
// Gated by protect_version::IsAtLeast(7,1,0) -- on older Protect we don't
// touch the schema.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"

struct pg_conn;
typedef struct pg_conn PGconn;  // forward decl to avoid pulling in libpq-fe.h

namespace onvif {
namespace event_recovery {

// One discovered backup file with its on-disk timestamp.
struct Backup {
  std::string path;        // absolute
  uint64_t    mtime_ms{0}; // file modification time (ms since epoch)
};

// Decide whether to trigger a recovery on this startup.  Logic:
//   - If protect_version::IsAtLeast(7,1,0) is false -> false (legacy Protect)
//   - Query oldest events.start and oldest recordingFiles.start.
//   - If oldest event is more than `threshold_ms` newer than oldest recording,
//     there is a gap that recordingFiles say should be populated -> true.
//   - If events table is empty -> true.
// Default threshold is 24h (giving slack for backup latency / time drift).
bool ShouldRecover(PGconn* conn, uint64_t threshold_ms = 86'400'000ULL);

// Test-friendly variant: pass the timestamps explicitly.  Production code
// uses the PGconn overload above.
bool ShouldRecoverFromTimestamps(int64_t oldest_event_ms,
                                  int64_t oldest_recording_ms,
                                  uint64_t threshold_ms = 86'400'000ULL);

// Scan @p dir for files matching `db_backup_partial.*.dump*` and return the
// most recent (highest mtime).  Returns empty optional if the directory does
// not exist or has no matching files.
std::optional<Backup> FindLatestBackup(const std::string& dir);

// Test-friendly variant: pick the newest entry from an explicit list.
std::optional<Backup> PickNewest(const std::vector<Backup>& candidates);

// Shell out to pg_restore (looking under common install paths for a 16+ binary
// that can read the dump.dl format).  Restores only the events, detectionLabels,
// and labels tables -- the only ones in the partial backup -- using
// `--data-only` and ON-CONFLICT-DO-NOTHING semantics.  Returns OK on success
// or a descriptive error.
absl::Status RestoreFromBackup(PGconn* conn, const std::string& dump_path);

// For every event whose metadata is missing the rich 7.1+ shape (no
// detectedAreas key), run event_enricher to populate metadata, set
// thumbnailFullfovId, and insert a paired smartDetectObjectAreas row per
// existing smartDetectObject.  Idempotent: re-running rewrites metadata to
// the same canonical synth.
absl::Status EnrichRestored(PGconn* conn,
                             int image_width = 2560,
                             int image_height = 1440);

// High-level orchestrator: ShouldRecover() -> FindLatestBackup() ->
// RestoreFromBackup() -> EnrichRestored().  Logs each step.  Safe to call
// unconditionally -- it gates on protect_version internally.
absl::Status Run(PGconn* conn,
                  const std::string& backups_dir = "/srv/unifi-protect/dbBackups");

}  // namespace event_recovery
}  // namespace onvif
