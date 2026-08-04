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

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "wedge_healer.hpp"

int main() {
  using onvif::WedgeHealer;

  // Case 1: brand-new process, no stats yet -> no wedge.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    WedgeHealer::MsrSnapshot msr;
    const auto r = h.tick_for_testing(/*db_ms_since_success=*/-1,
                                       /*db_ms_since_timeout=*/-1,
                                       msr);
    assert(r == WedgeHealer::Reason::kNone);
    assert(cmds.empty());
  }

  // Case 2: DB timeout very recent, no success in the window -> DB wedge.
  // But the fire is suppressed by the warmup gate (5 min), so no
  // command is executed on a freshly-constructed healer.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    WedgeHealer::MsrSnapshot msr;
    const auto r = h.tick_for_testing(/*db_ms_since_success=*/-1,
                                       /*db_ms_since_timeout=*/10,
                                       msr);
    assert(r == WedgeHealer::Reason::kDbWedge);
    assert(cmds.empty());  // suppressed by warmup
  }

  // Case 3: recent success AND recent timeout -> not wedged (we're still
  // making progress, some queries just happen to slow down).
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    WedgeHealer::MsrSnapshot msr;
    const auto r = h.tick_for_testing(/*db_ms_since_success=*/500,
                                       /*db_ms_since_timeout=*/500,
                                       msr);
    assert(r == WedgeHealer::Reason::kNone);
    assert(cmds.empty());
  }

  // Case 4: MSR failing continuously, no OK in the window -> MSR wedge.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    WedgeHealer::MsrSnapshot msr;
    msr.total_fail    = 100;
    msr.total_ok      = 5;
    msr.ms_since_fail = 5000;    // 5 s ago
    msr.ms_since_ok   = 120000;  // 2 min ago
    const auto r = h.tick_for_testing(/*db_ms_since_success=*/50,
                                       /*db_ms_since_timeout=*/-1,
                                       msr);
    assert(r == WedgeHealer::Reason::kMsrWedge);
    // Still in warmup so the command isn't executed on this fresh healer.
    assert(cmds.empty());
  }

  // Case 5: MSR failing recently but OK also recent -> not wedged.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    WedgeHealer::MsrSnapshot msr;
    msr.total_fail    = 10;
    msr.total_ok      = 100;
    msr.ms_since_fail = 5000;
    msr.ms_since_ok   = 1000;
    const auto r = h.tick_for_testing(/*db_ms_since_success=*/50,
                                       /*db_ms_since_timeout=*/-1,
                                       msr);
    assert(r == WedgeHealer::Reason::kNone);
    assert(cmds.empty());
  }

  // ---------------------------------------------------------------
  // featureFlags drift (issue #34)
  // ---------------------------------------------------------------

  // Case 6: nothing armed -> the check is a no-op even if the drift fn
  // would report drift.  The healer must never go looking on its own.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    bool called = false;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    h.set_flag_drift_fn([&](const std::vector<std::string>&) {
      called = true;
      return std::vector<std::string>{"cam1"};
    });
    const auto r = h.check_flag_drift_for_testing();
    assert(r == WedgeHealer::Reason::kNone);
    assert(!called);
    assert(cmds.empty());
  }

  // Case 7: armed, drift fn reports everything in sync -> no restart.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    h.set_flag_drift_fn([](const std::vector<std::string>&) {
      return std::vector<std::string>{};  // no drift
    });
    h.arm_flag_drift_check({"cam1", "cam2"});
    const auto r = h.check_flag_drift_for_testing();
    assert(r == WedgeHealer::Reason::kNone);
    assert(cmds.empty());
  }

  // Case 8: armed, drift reported -> kFlagDrift.  The restart itself is
  // suppressed by warmup on a fresh healer, but the reason is returned so
  // the caller (and the log) still record the detection.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    std::vector<std::string> seen_ids;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    h.set_flag_drift_fn([&](const std::vector<std::string>& ids) {
      seen_ids = ids;
      return std::vector<std::string>{"cam2"};
    });
    h.arm_flag_drift_check({"cam1", "cam2"});
    const auto r = h.check_flag_drift_for_testing();
    assert(r == WedgeHealer::Reason::kFlagDrift);
    assert(seen_ids.size() == 2);
    assert(cmds.empty());  // suppressed by warmup

    // Suppressed by a safeguard -> must have re-armed with the drifting
    // subset so the restart isn't lost.  Second check sees only cam2.
    seen_ids.clear();
    const auto r2 = h.check_flag_drift_for_testing();
    assert(r2 == WedgeHealer::Reason::kFlagDrift);
    assert(seen_ids.size() == 1 && seen_ids[0] == "cam2");
  }

  // Case 9: arming twice before the check runs merges both ID sets
  // rather than dropping the first.
  {
    WedgeHealer h;
    std::vector<std::string> seen_ids;
    h.set_exec_fn([&](const std::string&) { return 0; });
    h.set_flag_drift_fn([&](const std::vector<std::string>& ids) {
      seen_ids = ids;
      return std::vector<std::string>{};
    });
    h.arm_flag_drift_check({"cam1"});
    h.arm_flag_drift_check({"cam2", "cam1"});  // cam1 must not duplicate
    h.check_flag_drift_for_testing();
    assert(seen_ids.size() == 2);
  }

  // Case 10: the drift check is one-shot -- after it runs and finds
  // nothing, a second call does nothing until re-armed.
  {
    WedgeHealer h;
    int calls = 0;
    h.set_exec_fn([&](const std::string&) { return 0; });
    h.set_flag_drift_fn([&](const std::vector<std::string>&) {
      ++calls;
      return std::vector<std::string>{};
    });
    h.arm_flag_drift_check({"cam1"});
    h.check_flag_drift_for_testing();
    h.check_flag_drift_for_testing();
    assert(calls == 1);
  }

  std::printf("test_wedge_healer: OK\n");
  return 0;
}
