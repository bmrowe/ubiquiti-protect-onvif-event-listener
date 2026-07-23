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

#include "pg_stats.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/synchronization/mutex.h"

namespace onvif {
namespace pg {

namespace {

// Bucket per SQL fingerprint.  We keep a P95-approximation via a
// tumbling window of the 32 most recent samples -- exact P95 would need
// a t-digest / HDR-histogram; 32-sample rolling is plenty for a triage
// view.
struct Bucket {
  int64_t count       = 0;
  int64_t timeouts    = 0;
  int64_t total_us    = 0;
  int64_t min_us      = INT64_MAX;
  int64_t max_us      = 0;
  int64_t last_us     = 0;
  // Ring buffer of the last kRingCap samples.
  static constexpr int kRingCap = 32;
  int64_t ring[kRingCap] = {};
  int     ring_pos       = 0;
  int     ring_len       = 0;
};

absl::Mutex& mu() {
  static absl::Mutex m;
  return m;
}

std::unordered_map<std::string, Bucket>& buckets() {
  static auto* b = new std::unordered_map<std::string, Bucket>();
  return *b;
}

std::chrono::steady_clock::time_point& boot() {
  static auto t = std::chrono::steady_clock::now();
  return t;
}

// Timestamps of the most recent success / timeout, expressed as
// steady_clock time_points.  Default-constructed value == never seen.
absl::Mutex& health_mu() {
  static absl::Mutex m;
  return m;
}
std::chrono::steady_clock::time_point& last_success() {
  static std::chrono::steady_clock::time_point t{};
  return t;
}
std::chrono::steady_clock::time_point& last_timeout() {
  static std::chrono::steady_clock::time_point t{};
  return t;
}

// Fingerprint: first 60 chars of the SQL, with runs of whitespace
// collapsed to a single space and leading/trailing whitespace removed.
// Enough to distinguish "SELECT MAX(start) FROM events" from
// "SELECT id, name FROM cameras" while still bucketing the same query
// with different bound parameters together.
std::string fingerprint(const char* sql) {
  if (!sql) return "<null>";
  std::string out;
  out.reserve(64);
  bool in_ws = true;  // trims leading whitespace
  for (const char* p = sql; *p && out.size() < 60; ++p) {
    const char c = *p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!in_ws) { out += ' '; in_ws = true; }
    } else {
      out += c;
      in_ws = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

int64_t percentile(int64_t* samples, int n, double p) {
  if (n <= 0) return 0;
  std::vector<int64_t> v(samples, samples + n);
  std::sort(v.begin(), v.end());
  const int idx = std::min(
      n - 1,
      static_cast<int>(static_cast<double>(n) * p));
  return v[idx];
}

std::string escape_json(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

void RecordQueryStats(const char* sql,
                      int64_t micros,
                      bool timed_out) {
  const std::string fp = fingerprint(sql);
  (void)boot();  // ensure boot time is set
  const auto now = std::chrono::steady_clock::now();
  {
    absl::MutexLock lk(&mu());
    Bucket& b = buckets()[fp];
    ++b.count;
    if (timed_out) ++b.timeouts;
    b.total_us += micros;
    if (micros < b.min_us) b.min_us = micros;
    if (micros > b.max_us) b.max_us = micros;
    b.last_us = micros;
    b.ring[b.ring_pos] = micros;
    b.ring_pos = (b.ring_pos + 1) % Bucket::kRingCap;
    if (b.ring_len < Bucket::kRingCap) ++b.ring_len;
  }
  {
    absl::MutexLock lk(&health_mu());
    if (timed_out)
      last_timeout() = now;
    else
      last_success() = now;
  }
}

int64_t MsSinceLastSuccess() {
  absl::MutexLock lk(&health_mu());
  if (last_success() == std::chrono::steady_clock::time_point{}) return -1;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - last_success()).count();
}

int64_t MsSinceLastTimeout() {
  absl::MutexLock lk(&health_mu());
  if (last_timeout() == std::chrono::steady_clock::time_point{}) return -1;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - last_timeout()).count();
}

std::string StatsAsJson(int top_n) {
  const auto now = std::chrono::steady_clock::now();
  const int64_t since_boot_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now - boot()).count();

  // Copy buckets while holding the lock, then rank + emit outside.
  std::vector<std::pair<std::string, Bucket>> snapshot;
  {
    absl::MutexLock lk(&mu());
    snapshot.reserve(buckets().size());
    for (const auto& kv : buckets()) snapshot.push_back(kv);
  }

  std::sort(snapshot.begin(), snapshot.end(),
    [](const auto& a, const auto& b) {
      return a.second.total_us > b.second.total_us;
    });
  if (top_n > 0 && static_cast<int>(snapshot.size()) > top_n)
    snapshot.resize(top_n);

  std::string j = "{\"since_boot_ms\":";
  j += std::to_string(since_boot_ms);
  j += ",\"queries\":[";
  bool first = true;
  for (auto& kv : snapshot) {
    Bucket& b = kv.second;
    const int64_t avg_us = b.count > 0 ? b.total_us / b.count : 0;
    const int64_t p95_us = percentile(b.ring, b.ring_len, 0.95);
    if (!first) j += ',';
    first = false;
    j += "{\"sql\":\"";     j += escape_json(kv.first);
    j += "\",\"count\":";   j += std::to_string(b.count);
    j += ",\"timeouts\":";  j += std::to_string(b.timeouts);
    j += ",\"total_ms\":";  j += std::to_string(b.total_us / 1000);
    j += ",\"avg_ms\":";    j += std::to_string(avg_us / 1000);
    j += ",\"min_ms\":";    j += std::to_string(
        b.min_us == INT64_MAX ? 0 : b.min_us / 1000);
    j += ",\"max_ms\":";    j += std::to_string(b.max_us / 1000);
    j += ",\"p95_ms\":";    j += std::to_string(p95_us / 1000);
    j += ",\"last_ms\":";   j += std::to_string(b.last_us / 1000);
    j += "}";
  }
  j += "]}";
  return j;
}

void ResetStats() {
  absl::MutexLock lk(&mu());
  buckets().clear();
  boot() = std::chrono::steady_clock::now();
}

}  // namespace pg
}  // namespace onvif
