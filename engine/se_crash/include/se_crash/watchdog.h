// Copyright 2026 The simple-engine-2 Authors.
//
// The watchdog thread (docs/design/mt-design.md "watchdog"; the M4
// skeleton of docs/milestone-specs.md "Integration (M4a + M4b + M5)"; M12
// finishes hang detection). Role kWatchdog, one per process, started by the
// App in boot order and joined last.
//
//   auto watchdog = se::crash::Watchdog::Start({.inputs = inputs});
//   watchdog->Arm();                 // after the first iterate
//   watchdog->RequestDump("reason"); // one dump, from the watchdog thread
//   watchdog->StartShutdownDeadline();
//   watchdog->Stop();                // the last engine thread to end
//
// It wakes on a binary_semaphore with a 1 s timeout (a request, the
// deadline start and the stop also wake it) and measures time on the
// unbiased clock (QueryUnbiasedInterruptTimePrecise, CLOCK_MONOTONIC),
// which excludes system sleep. Each period it reads main's iterate counter
// and the Suspended, Modal and heartbeat flags (atomics only) into the pure
// HangDetector: the 30 s boot budget applies until Arm(), and hang
// declaration stays off until M12. Every dump is written out of process by
// the crash handler (DumpWithoutCrash); the watchdog never calls DbgHelp
// and never breaks into a debugger. When the shutdown deadline (5 s after
// the quit drain) expires it requests a dump, has the log worker flush and
// ends the process.

#ifndef SE_CRASH_WATCHDOG_H_
#define SE_CRASH_WATCHDOG_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include "se_core/error.h"
#include "se_crash/hang_detector.h"

namespace se::crash {

// What the watchdog reads (docs/design/mt-design.md "Data Flow": main's
// iterate counter, relaxed; the Suspended, Modal and heartbeat flags,
// release stores). Each must outlive the watchdog; a null one reads as 0
// or false.
struct WatchdogInputs {
  const std::atomic<std::uint64_t>* iterate_counter = nullptr;
  const std::atomic<bool>* modal = nullptr;
  const std::atomic<bool>* suspended = nullptr;
  const std::atomic<bool>* heartbeat = nullptr;
};

// Writes one dump; DumpWithoutCrash unless a test substitutes its own.
using DumpFunction = std::expected<void, Error> (*)(std::string_view reason);

// The exit code of a process the shutdown deadline ended.
inline constexpr int kShutdownDeadlineExitCode = 3;
// Dump requests that may wait for the watchdog thread.
inline constexpr std::uint32_t kDumpRequestQueue = 8;
// The longest reason a request keeps.
inline constexpr std::size_t kDumpRequestReasonSize = 120;

struct WatchdogConfig {
  WatchdogInputs inputs{};
  // The watchdog wakes at least this often (the hang period).
  std::uint32_t period_ms = 1000;
  HangDetectorConfig detector{};
  // When the boot started (UnbiasedNowNs()); 0: when the watchdog starts.
  std::uint64_t boot_start_ns = 0;
  // From StartShutdownDeadline() to the dump, the log flush and the end of
  // the process.
  std::uint64_t shutdown_deadline_ns = 5'000'000'000;
  // End the process when the deadline expires (tests turn it off and read
  // Stats().deadline_expired).
  bool terminate_on_deadline = true;
  // nullptr: DumpWithoutCrash.
  DumpFunction dump = nullptr;
};

struct WatchdogStats {
  // Periods judged by the HangDetector.
  std::uint64_t periods = 0;
  std::uint64_t stalled_periods = 0;
  std::uint64_t dumps_written = 0;
  std::uint64_t dumps_failed = 0;
  // RequestDump calls refused because the queue was full.
  std::uint64_t requests_dropped = 0;
  bool armed = false;
  bool boot_over_budget = false;
  bool deadline_started = false;
  bool deadline_expired = false;
};

class Watchdog {
 public:
  // Starts the watchdog thread. Fails when the thread cannot start.
  [[nodiscard]] static std::expected<Watchdog, Error> Start(
      const WatchdogConfig& config);

  Watchdog(Watchdog&& other) noexcept;
  Watchdog& operator=(Watchdog&& other) noexcept;
  // Stops and joins the thread if it still runs.
  ~Watchdog();

  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;

  // The first iterate ran: the boot budget ends and progress is measured
  // from now on. Main only.
  void Arm() noexcept;

  // Asks the watchdog thread for one dump with |reason| (truncated to
  // kDumpRequestReasonSize bytes). Main only (an SPSC queue); returns
  // false when the queue is full or the watchdog has stopped. The dump is
  // written on the watchdog thread; Stats().dumps_written counts it.
  bool RequestDump(std::string_view reason) noexcept;

  // Shutdown step 3: the quit drain is over and the deadline starts.
  void StartShutdownDeadline() noexcept;

  // Requests a stop, serves the dump requests still queued and joins.
  // Idempotent.
  void Stop() noexcept;

  [[nodiscard]] bool Running() const noexcept;
  [[nodiscard]] WatchdogStats Stats() const noexcept;

 private:
  struct State;
  explicit Watchdog(std::unique_ptr<State> state) noexcept;

  std::unique_ptr<State> state_;
};

}  // namespace se::crash

#endif  // SE_CRASH_WATCHDOG_H_
