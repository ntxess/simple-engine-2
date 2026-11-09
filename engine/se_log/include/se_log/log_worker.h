// Copyright 2026 The simple-engine-2 Authors.
//
// LogWorker: the log worker thread (docs/design/mt-design.md "log worker").
// It owns the spdlog sinks (a rotating file, stderr and the in-memory panel
// ring of M10's log panel), used synchronously on the worker only, and
// drains the per-thread LogRings the SE_LOG_* front end writes
// (se_core/log.h, se_core/log_ring.h):
//
//   auto logs = se::LogWorker::Start({.file_path = "logs/game.log"});
//   ...
//   logs->Stop();  // before main returns
//
// The worker runs as role kLogWorker at default QoS. It wakes on a
// binary_semaphore released by the first record pushed after it parks, or
// every 50 ms. Each wake drains every registered ring and the library ring,
// merges the records by (timestamp, sequence) and writes them, holding back
// any record that a write still in progress on another thread could
// precede (ADR-032). A full ring drops the new record and counts it; the
// worker then logs "N records dropped on <thread>" once per wake. A call
// site (file, line) that logs more than 50 records per second is cut to a
// summary line per second. Lines look like
//
//   12.345678 I main t=1440 schedule.cc:88 loaded 17 entities
//
// (seconds since LogEpochNs(), level, role, tick, location, message).
//
// Start configures the sinks before any other engine thread exists. Stop
// drains everything, flushes, and calls spdlog::shutdown() (which on
// Windows must happen before main returns); the front end writes
// synchronously from then on. se_log is the only module that includes
// spdlog (tools/checks/check_includes.py).

#ifndef SE_LOG_LOG_WORKER_H_
#define SE_LOG_LOG_WORKER_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "se_core/dump_stream.h"
#include "se_core/error.h"
#include "se_core/log.h"

namespace se {

struct LogConfig {
  // The log file, rotated at file_max_bytes with file_max_files kept;
  // empty: no file.
  std::string file_path{};
  std::size_t file_max_bytes = std::size_t{5} * 1024 * 1024;
  std::size_t file_max_files = 3;
  // stderr, from stderr_level up.
  bool stderr_enabled = true;
  LogLevel stderr_level = LogLevel::kInfo;
  // Lines the panel ring keeps; 0: no panel.
  std::size_t panel_lines = 2000;
  // The worker wakes at least this often.
  std::uint32_t wake_period_ms = 50;
  // Whether a push wakes a parked worker. Off only in tests that let the
  // rings fill up.
  bool wake_on_push = true;
  // Records one call site may write per second; 0: no limit.
  std::uint32_t rate_limit_per_second = 50;
};

struct LogWorkerStats {
  // Ring records consumed.
  std::uint64_t records = 0;
  // Lines written: records, drop notices and rate-limit summaries.
  std::uint64_t lines = 0;
  // Records the rate limit cut.
  std::uint64_t suppressed = 0;
  // Ring drops reported in notices.
  std::uint64_t dropped = 0;
  // Sink calls that failed (spdlog_ex), counted and survived.
  std::uint64_t sink_errors = 0;
  // Passes of the worker loop that ended in a park.
  std::uint64_t parks = 0;
};

class LogWorker {
 public:
  // Creates the sinks, attaches the worker to the front end and starts its
  // thread. Fails when the file cannot be opened, another worker runs or
  // the thread cannot start.
  [[nodiscard]] static std::expected<LogWorker, Error> Start(
      const LogConfig& config);

  LogWorker(LogWorker&& other) noexcept;
  LogWorker& operator=(LogWorker&& other) noexcept;
  // Stops the worker if it still runs.
  ~LogWorker();

  LogWorker(const LogWorker&) = delete;
  LogWorker& operator=(const LogWorker&) = delete;

  // Drains every ring, writes, flushes, joins the thread and shuts spdlog
  // down. The front end writes synchronously afterwards. Idempotent.
  void Stop() noexcept;

  [[nodiscard]] bool Running() const noexcept;

  // Copies the panel ring's lines, oldest first. Never waits: returns false,
  // leaving |lines| alone, while the worker holds the panel (the main
  // thread takes shared locks only with try_lock).
  bool TryCopyPanelLines(std::vector<std::string>& lines) const;

  [[nodiscard]] LogWorkerStats Stats() const noexcept;

 private:
  struct State;
  explicit LogWorker(std::unique_ptr<State> state) noexcept;

  std::unique_ptr<State> state_;
};

// Formats |record| as the worker writes it (without a newline) into |out|.
void FormatLogLine(const LogRecord& record, std::string& out);

}  // namespace se

#endif  // SE_LOG_LOG_WORKER_H_
