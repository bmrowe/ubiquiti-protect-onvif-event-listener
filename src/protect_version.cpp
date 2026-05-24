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

#include "protect_version.hpp"

#include <atomic>
#include <cstring>

namespace onvif {
namespace protect_version {

namespace {

// We pack {major, minor, patch} into a single uint32_t so the runtime read
// is a lock-free atomic load on every supported platform:
//   bits 24..31 = major  (8 bits, plenty for the foreseeable future)
//   bits 16..23 = minor
//   bits  0..15 = patch  (16 bits — Protect's patch numbers can climb high)
// SetCurrent clamps each field at its width to keep the pack safe; out-of-
// range fields cap at the max for that width.  No real Protect version comes
// close to those caps in practice.
constexpr uint32_t kMajorMask = 0xFFu;
constexpr uint32_t kMinorMask = 0xFFu;
constexpr uint32_t kPatchMask = 0xFFFFu;
constexpr int      kMajorShift = 24;
constexpr int      kMinorShift = 16;

uint32_t Pack(const Version& v) {
  uint32_t maj = static_cast<uint32_t>(v.major < 0 ? 0 : v.major) & kMajorMask;
  uint32_t min = static_cast<uint32_t>(v.minor < 0 ? 0 : v.minor) & kMinorMask;
  uint32_t pat = static_cast<uint32_t>(v.patch < 0 ? 0 : v.patch) & kPatchMask;
  return (maj << kMajorShift) | (min << kMinorShift) | pat;
}

Version Unpack(uint32_t bits) {
  Version v;
  v.major = static_cast<int>((bits >> kMajorShift) & kMajorMask);
  v.minor = static_cast<int>((bits >> kMinorShift) & kMinorMask);
  v.patch = static_cast<int>(bits & kPatchMask);
  return v;
}

std::atomic<uint32_t>& Slot() {
  static std::atomic<uint32_t> s{0};
  return s;
}

}  // namespace

std::optional<Version> Parse(std::string_view s) {
  // Skip leading whitespace.
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);

  auto parse_int = [](std::string_view& sv, int* out) -> bool {
    if (sv.empty() || sv.front() < '0' || sv.front() > '9') return false;
    int n = 0;
    while (!sv.empty() && sv.front() >= '0' && sv.front() <= '9') {
      // Guard against extreme overflow; we don't expect anything close.
      if (n > 100000) return false;
      n = n * 10 + (sv.front() - '0');
      sv.remove_prefix(1);
    }
    *out = n;
    return true;
  };
  auto eat = [](std::string_view& sv, char c) -> bool {
    if (sv.empty() || sv.front() != c) return false;
    sv.remove_prefix(1);
    return true;
  };

  Version v;
  if (!parse_int(s, &v.major)) return std::nullopt;
  if (!eat(s, '.'))            return std::nullopt;
  if (!parse_int(s, &v.minor)) return std::nullopt;
  if (!eat(s, '.'))            return std::nullopt;
  if (!parse_int(s, &v.patch)) return std::nullopt;
  // Anything trailing ("-foo", ".N", whitespace, ...) is ignored.
  return v;
}

void SetCurrent(Version v) {
  Slot().store(Pack(v), std::memory_order_relaxed);
}

Version Current() noexcept {
  return Unpack(Slot().load(std::memory_order_relaxed));
}

void ResetForTesting() noexcept {
  Slot().store(0, std::memory_order_relaxed);
}

}  // namespace protect_version
}  // namespace onvif
