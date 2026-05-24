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

// Runtime gate for the running unifi-protect controller's version.
//
// `protect_ui_patch::patch_alarm_picker()` calls `SetCurrent()` at startup
// after parsing `dpkg-query`'s output.  Live event writers (detection_recorder,
// motion_poller) call `IsAtLeast()` to choose between the legacy sparse event
// shape and the rich 7.1+ shape.  The atomic backing makes `IsAtLeast()` safe
// to call from any thread without locking.

#include <cstdint>
#include <optional>
#include <string_view>

namespace onvif {
namespace protect_version {

struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;

  // Strict less-than for (major, minor, patch).
  constexpr bool operator<(const Version& o) const noexcept {
    if (major != o.major) return major < o.major;
    if (minor != o.minor) return minor < o.minor;
    return patch < o.patch;
  }
  constexpr bool operator==(const Version& o) const noexcept {
    return major == o.major && minor == o.minor && patch == o.patch;
  }
};

// Parse a "MAJOR.MINOR.PATCH..." prefix.  Tolerates trailing
// "-suffix", ".extra", or whitespace.  Returns empty optional only if there
// is no MAJOR.MINOR.PATCH triple at the start of the string.
std::optional<Version> Parse(std::string_view s);

// Publish the running version into a process-global atomic.  Safe to call
// from any thread.  Subsequent reads via Current() / IsAtLeast() see the
// new value.
void SetCurrent(Version v);

// Read the published version.  If SetCurrent() has never been called this
// returns {0,0,0} (which is older than any real release, so the
// pre-7.1 path is the safe default for IsAtLeast(7,1,0)).
Version Current() noexcept;

// Convenience predicate.  Equivalent to !(Current() < {maj,min,patch}).
inline bool IsAtLeast(int major, int minor, int patch) noexcept {
  Version v = Current();
  return !(v < Version{major, minor, patch});
}

// Test hook: clear the current version back to {0,0,0}.  Production code
// must never call this; tests use it to reset state between cases.
void ResetForTesting() noexcept;

}  // namespace protect_version
}  // namespace onvif
