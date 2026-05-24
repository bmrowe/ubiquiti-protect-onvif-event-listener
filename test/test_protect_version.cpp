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

#include <cstdio>
#include <cstdlib>

#include "protect_version.hpp"

namespace {

#define CHECK(expr) do { \
  if (!(expr)) { \
    std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                 __FILE__, __LINE__, #expr); \
    std::exit(1); \
  } \
} while (0)

void test_parse_simple() {
  auto v = onvif::protect_version::Parse("7.1.47");
  CHECK(v.has_value());
  CHECK(v->major == 7 && v->minor == 1 && v->patch == 47);
}

void test_parse_with_dotted_suffix() {
  auto v = onvif::protect_version::Parse("7.0.57.1");
  CHECK(v.has_value());
  CHECK(v->major == 7 && v->minor == 0 && v->patch == 57);
}

void test_parse_with_dash_suffix() {
  auto v = onvif::protect_version::Parse("7.1.47-beta");
  CHECK(v.has_value());
  CHECK(v->major == 7 && v->minor == 1 && v->patch == 47);
}

void test_parse_with_leading_whitespace() {
  auto v = onvif::protect_version::Parse("  7.1.47\n");
  CHECK(v.has_value());
  CHECK(v->patch == 47);
}

void test_parse_rejects_empty() {
  CHECK(!onvif::protect_version::Parse("").has_value());
}

void test_parse_rejects_two_components() {
  CHECK(!onvif::protect_version::Parse("7.1").has_value());
}

void test_parse_rejects_non_numeric() {
  CHECK(!onvif::protect_version::Parse("v7.1.47").has_value());
  CHECK(!onvif::protect_version::Parse("seven.one.fortyseven").has_value());
}

void test_is_at_least_default_is_zero() {
  onvif::protect_version::ResetForTesting();
  CHECK(onvif::protect_version::Current().major == 0);
  // Anything < 0 doesn't exist; this is testing that the default state
  // (no SetCurrent called yet) means IsAtLeast for any real version is false.
  CHECK(!onvif::protect_version::IsAtLeast(7, 1, 0));
  CHECK(!onvif::protect_version::IsAtLeast(1, 0, 0));
  CHECK(onvif::protect_version::IsAtLeast(0, 0, 0));
}

void test_is_at_least_after_set() {
  onvif::protect_version::ResetForTesting();
  onvif::protect_version::SetCurrent({7, 1, 47});

  // True for everything <= 7.1.47.
  CHECK(onvif::protect_version::IsAtLeast(7, 1, 47));
  CHECK(onvif::protect_version::IsAtLeast(7, 1, 0));
  CHECK(onvif::protect_version::IsAtLeast(7, 0, 100));
  CHECK(onvif::protect_version::IsAtLeast(6, 99, 99));

  // False for anything > 7.1.47.
  CHECK(!onvif::protect_version::IsAtLeast(7, 1, 48));
  CHECK(!onvif::protect_version::IsAtLeast(7, 2, 0));
  CHECK(!onvif::protect_version::IsAtLeast(8, 0, 0));
}

void test_seven_one_threshold() {
  // The migration threshold the rest of the project depends on.
  onvif::protect_version::ResetForTesting();

  // 7.0.x and 7.0.57.1 should NOT be at least 7.1.0.
  onvif::protect_version::SetCurrent(*onvif::protect_version::Parse("7.0.57"));
  CHECK(!onvif::protect_version::IsAtLeast(7, 1, 0));
  onvif::protect_version::SetCurrent(*onvif::protect_version::Parse("7.0.57.1"));
  CHECK(!onvif::protect_version::IsAtLeast(7, 1, 0));

  // 7.1.0, 7.1.47, 7.2.0 should all be at least 7.1.0.
  onvif::protect_version::SetCurrent(*onvif::protect_version::Parse("7.1.0"));
  CHECK(onvif::protect_version::IsAtLeast(7, 1, 0));
  onvif::protect_version::SetCurrent(*onvif::protect_version::Parse("7.1.47"));
  CHECK(onvif::protect_version::IsAtLeast(7, 1, 0));
  onvif::protect_version::SetCurrent(*onvif::protect_version::Parse("7.2.0"));
  CHECK(onvif::protect_version::IsAtLeast(7, 1, 0));
}

void test_set_current_round_trip() {
  onvif::protect_version::ResetForTesting();
  onvif::protect_version::SetCurrent({12, 34, 5678});
  auto v = onvif::protect_version::Current();
  CHECK(v.major == 12);
  CHECK(v.minor == 34);
  CHECK(v.patch == 5678);
}

}  // namespace

int main() {
  test_parse_simple();
  test_parse_with_dotted_suffix();
  test_parse_with_dash_suffix();
  test_parse_with_leading_whitespace();
  test_parse_rejects_empty();
  test_parse_rejects_two_components();
  test_parse_rejects_non_numeric();
  test_is_at_least_default_is_zero();
  test_is_at_least_after_set();
  test_seven_one_threshold();
  test_set_current_round_trip();
  std::printf("protect_version tests passed\n");
  return 0;
}
