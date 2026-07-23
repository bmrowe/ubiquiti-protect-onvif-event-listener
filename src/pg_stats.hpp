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

#ifndef SRC_PG_STATS_HPP_
#define SRC_PG_STATS_HPP_

#include <cstdint>
#include <string>

namespace onvif {
namespace pg {

// Records elapsed time for a single query.  Called from pg_util's
// ExecWithTimeout / ExecParamsWithTimeout wrappers so every SQL round-trip
// contributes.  @p sql is the raw SQL text; this function derives a
// stable per-shape fingerprint (first ~60 chars, whitespace-normalised)
// so semantically-identical queries with different bound parameters
// aggregate into the same bucket.  Thread-safe.
void RecordQueryStats(const char* sql,
                      int64_t micros,
                      bool timed_out);

// Wall-clock milliseconds since the last successful query completed.
// Returns -1 if no query has ever completed successfully (i.e. we
// haven't seen a healthy DB yet).  Used by the wedge healer to decide
// whether the DB has been unresponsive for long enough to justify a
// service restart.
int64_t MsSinceLastSuccess();

// Wall-clock milliseconds since the last query timeout.  Returns -1 if
// no timeout has ever been observed.  A "wedge" is
// MsSinceLastTimeout()  <  60000  &&  MsSinceLastSuccess() > 60000.
int64_t MsSinceLastTimeout();

// JSON snapshot of the current stats bucket, sorted by total_micros
// descending.  Shape:
//   { "since_boot_ms": ...,
//     "queries": [
//       { "sql": "<fingerprint>",
//         "count": N, "timeouts": M,
//         "total_ms": T, "avg_ms": A,
//         "min_ms": ..., "max_ms": ..., "p95_ms": ... },
//       ...
//     ] }
// @p top_n caps the returned rows (0 = no cap).
std::string StatsAsJson(int top_n);

// Reset all counters.  Used by /api/pg_stats?reset=1 for
// before/after comparisons.
void ResetStats();

}  // namespace pg
}  // namespace onvif

#endif  // SRC_PG_STATS_HPP_
