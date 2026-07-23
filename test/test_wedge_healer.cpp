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

  std::printf("test_wedge_healer: OK\n");
  return 0;
}
