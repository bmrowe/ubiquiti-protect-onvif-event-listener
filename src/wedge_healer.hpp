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

#ifndef SRC_WEDGE_HEALER_HPP_
#define SRC_WEDGE_HEALER_HPP_

#include <atomic>
#include <chrono>  // NOLINT(build/c++11)
#include <cstdint>
#include <functional>
#include <mutex>  // NOLINT(build/c++11)
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

namespace onvif {

// Background thread that watches for wedged Protect state and issues a
// targeted `systemctl restart` when it fires.  There are two independent
// wedge signals:
//
//   * DB wedge   -- pg_stats has seen at least one query timeout in the
//                    last kWindowSec and zero successful queries in the
//                    same window.  Restarts msp + msr + unifi-protect.
//
//   * MSR wedge  -- msr_client has been failing continuously for
//                    kWindowSec with no successes.  Restarts msr only
//                    (less disruptive than the DB path).
//
// Rate limits protect against restart storms: no restart in the first
// kWarmupSec after our own start, at most one restart per kCooldownSec,
// and a hard 24 h cap.
class WedgeHealer {
 public:
  static constexpr int kWindowSec   = 60;
  static constexpr int kWarmupSec   = 300;   // 5 min
  static constexpr int kCooldownSec = 300;   // 5 min
  static constexpr int kMaxPerDay   = 4;

  // Callback to fetch the current MSR success / failure snapshot from
  // detection_recorder.  Called on the monitor thread with no locks
  // held; must be cheap + thread-safe.
  //   msr_ok_since_boot   - total OK calls since process start
  //   msr_fail_since_boot - total failed calls since process start
  //   msr_ms_since_last_ok / _fail - milliseconds since the most recent
  //                                 OK / fail (-1 if none yet)
  struct MsrSnapshot {
    uint64_t total_ok       = 0;
    uint64_t total_fail     = 0;
    int64_t  ms_since_ok    = -1;
    int64_t  ms_since_fail  = -1;
  };
  using MsrSnapshotFn = std::function<MsrSnapshot()>;

  // Callback that actually runs a shell command (e.g. `systemctl
  // restart ...`).  Injectable so tests can capture the invocation
  // without actually restarting services.  Should return the shell
  // exit code.
  using ExecFn = std::function<int(const std::string&)>;

  // Reasons a restart fires; also used as the string identifier in the
  // ERROR log line so downstream grep-based dashboards can tag them.
  enum class Reason {
    kNone,
    kDbWedge,
    kMsrWedge,
  };
  struct RestartRecord {
    Reason      reason;
    int64_t     ms_since_boot;   // when it fired
    std::string action;          // command line executed
    int         exit_code;
  };

  WedgeHealer();
  ~WedgeHealer();

  // Attach dependencies before start().
  void set_msr_snapshot_fn(MsrSnapshotFn fn) { msr_snapshot_ = std::move(fn); }
  void set_exec_fn(ExecFn fn) { exec_ = std::move(fn); }

  // Spawn the monitor thread.  Idempotent; safe to call once.
  void start();

  // Signal + join.
  void stop();

  // Number of restarts we've issued this run.  Used by the admin
  // /api/healer_status endpoint.
  uint64_t restart_count() const { return restart_count_.load(); }

  // Snapshot of the most recent restart records (up to kMaxPerDay).
  // Copies under a mutex; safe from any thread.
  std::vector<RestartRecord> recent_restarts() const;

  // Exposed for unit tests: run one monitor tick against injected
  // snapshots.  Returns the reason if it fired a restart this tick.
  Reason tick_for_testing(int64_t db_ms_since_success,
                          int64_t db_ms_since_timeout,
                          const MsrSnapshot& msr);

 private:
  void run();
  Reason decide_wedge(int64_t db_ms_since_success,
                      int64_t db_ms_since_timeout,
                      const MsrSnapshot& msr) const;
  void fire_restart(Reason r);

  MsrSnapshotFn                     msr_snapshot_;
  ExecFn                            exec_;
  std::atomic<bool>                 running_{false};
  std::thread                       thread_;
  std::atomic<uint64_t>             restart_count_{0};
  std::chrono::steady_clock::time_point boot_{};
  std::chrono::steady_clock::time_point last_restart_{};
  // History under a small mutex; recorded newest-last then trimmed.
  mutable std::mutex                history_mu_;
  std::vector<RestartRecord>        history_;
  // MSR snapshot at last successful monitor tick, used to detect that
  // OK counter advanced (== healthy) in the current window.
  MsrSnapshot                       last_msr_seen_{};
  std::chrono::steady_clock::time_point last_msr_ok_advance_{};
};

}  // namespace onvif

#endif  // SRC_WEDGE_HEALER_HPP_
