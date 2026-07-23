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

#include "wedge_healer.hpp"

#include <cstdlib>
#include <mutex>  // NOLINT(build/c++11)
#include <string>
#include <vector>

#include "absl/log/log.h"
#include "pg_stats.hpp"

namespace onvif {

namespace {

constexpr const char* kRestartCmdDbWedge = "systemctl restart msp.service msr.service unifi-protect.service";  // NOLINT(whitespace/line_length)

// MSR wedge is scoped to the msr binary only: it is the least
// disruptive service to bounce and playback stays functional.
constexpr const char* kRestartCmdMsrWedge = "systemctl restart msr.service";

const char* reason_str(WedgeHealer::Reason r) {
  switch (r) {
    case WedgeHealer::Reason::kDbWedge:  return "DB";
    case WedgeHealer::Reason::kMsrWedge: return "MSR";
    default:                             return "none";
  }
}

int default_exec(const std::string& cmd) {
  // std::system inherits the parent's stdio; that's fine here -- the
  // command is a systemctl invocation which prints little on success.
  return std::system(cmd.c_str());
}

}  // namespace

WedgeHealer::WedgeHealer()
  : exec_(default_exec),
    boot_(std::chrono::steady_clock::now()) {}  // NOLINT(whitespace/indent_namespace)

WedgeHealer::~WedgeHealer() { stop(); }

void WedgeHealer::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void WedgeHealer::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

std::vector<WedgeHealer::RestartRecord> WedgeHealer::recent_restarts() const {
  std::lock_guard<std::mutex> lk(history_mu_);
  return history_;
}

WedgeHealer::Reason WedgeHealer::decide_wedge(
    int64_t db_ms_since_success,
    int64_t db_ms_since_timeout,
    const MsrSnapshot& msr) const {
  const int64_t win_ms = kWindowSec * 1000LL;

  // DB wedge: timeout observed within the window AND no successful
  // query completed within the window.  Both -1 means "no data yet",
  // which we treat as not wedged (during startup / warmup).
  if (db_ms_since_timeout >= 0 && db_ms_since_timeout < win_ms) {
    const bool no_recent_success =
        db_ms_since_success < 0 || db_ms_since_success >= win_ms;
    if (no_recent_success) return Reason::kDbWedge;
  }

  // MSR wedge: total_fail advanced but total_ok did not, and the last
  // OK is older than the window.  The gap between last_msr_seen_ and
  // the current snapshot tells us "activity happened this window."
  //
  // We only consider it wedged if there have been failures in the
  // window (ms_since_fail < win_ms) *and* no successes in that time
  // (ms_since_ok < 0 or >= win_ms).
  if (msr.ms_since_fail >= 0 && msr.ms_since_fail < win_ms) {
    const bool no_recent_ok =
        msr.ms_since_ok < 0 || msr.ms_since_ok >= win_ms;
    if (no_recent_ok) return Reason::kMsrWedge;
  }

  return Reason::kNone;
}

void WedgeHealer::fire_restart(Reason r) {
  const auto now = std::chrono::steady_clock::now();
  const int64_t warmup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - boot_).count();
  if (warmup_ms < kWarmupSec * 1000LL) {
    LOG(WARNING) << "[healer] " << reason_str(r)
                 << " wedge detected but suppressed (warmup, "
                 << warmup_ms / 1000 << "s < " << kWarmupSec << "s)";
    return;
  }
  if (last_restart_ != std::chrono::steady_clock::time_point{}) {
    const int64_t since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_restart_).count();
    if (since_ms < kCooldownSec * 1000LL) {
      LOG(WARNING) << "[healer] " << reason_str(r)
                   << " wedge detected but suppressed (cooldown, "
                   << since_ms / 1000 << "s < " << kCooldownSec << "s)";
      return;
    }
  }
  if (restart_count_.load() >= kMaxPerDay) {
    LOG(WARNING) << "[healer] " << reason_str(r)
                 << " wedge detected but suppressed (24 h cap reached: "
                 << restart_count_.load() << "/" << kMaxPerDay << ")";
    return;
  }

  const std::string cmd = (r == Reason::kDbWedge)
      ? kRestartCmdDbWedge : kRestartCmdMsrWedge;
  LOG(ERROR) << "[healer] " << reason_str(r)
             << " wedged for >" << kWindowSec
             << "s -- running: " << cmd;
  const int rc = exec_ ? exec_(cmd) : -1;
  if (rc != 0) {
    LOG(ERROR) << "[healer] restart exit code " << rc
               << " (command: " << cmd << ")";
  } else {
    LOG(WARNING) << "[healer] Protect services restarted successfully";
  }
  last_restart_ = now;
  restart_count_.fetch_add(1);
  {
    std::lock_guard<std::mutex> lk(history_mu_);
    history_.push_back(RestartRecord{r, warmup_ms, cmd, rc});
    if (history_.size() > static_cast<size_t>(kMaxPerDay))
      history_.erase(history_.begin());
  }
}

WedgeHealer::Reason WedgeHealer::tick_for_testing(
    int64_t db_ms_since_success,
    int64_t db_ms_since_timeout,
    const MsrSnapshot& msr) {
  const Reason r = decide_wedge(db_ms_since_success,
                                 db_ms_since_timeout, msr);
  if (r != Reason::kNone) fire_restart(r);
  return r;
}

void WedgeHealer::run() {
  // Sleep in 1-second slices so stop() is prompt.  Tick cadence: 10 s.
  constexpr int kTickSec = 10;
  int tick_ctr = 0;
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!running_.load()) break;
    if (++tick_ctr < kTickSec) continue;
    tick_ctr = 0;

    const MsrSnapshot msr =
        msr_snapshot_ ? msr_snapshot_() : MsrSnapshot{};
    const int64_t db_ms_success = pg::MsSinceLastSuccess();
    const int64_t db_ms_timeout = pg::MsSinceLastTimeout();
    const Reason r = decide_wedge(db_ms_success, db_ms_timeout, msr);
    if (r != Reason::kNone) fire_restart(r);
  }
}

}  // namespace onvif
