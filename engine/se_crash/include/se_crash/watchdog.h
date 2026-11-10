// Copyright 2026 The simple-engine-2 Authors.
//
// The watchdog thread (docs/design/mt-design.md "watchdog"; the M4
// skeleton of docs/milestone-specs.md "Integration (M4a + M4b + M5)"; M12's
// hang declaration). Role kWatchdog, one per process, started by the App in
// boot order and joined last.
//
//   auto watchdog = se::crash::Watchdog::Start({.inputs = inputs});
//   watchdog->Arm();                 // after the first iterate
//   watchdog->RequestDump("reason"); // one dump, from the watchdog thread
//   watchdog->StartShutdownDeadline();
//   watchdog->Stop();                // the last engine thread to end
//
// It wakes on a binary_semaphore with a timeout of a quarter period (a
// request, the deadline start and the stop also wake it) and measures time
// on the unbiased clock (QueryUnbiasedInterruptTimePrecise, CLOCK_MONOTONIC),
// which excludes system sleep. Each wake reads main's iterate counter and
// the Suspended, Modal, heartbeat and in-driver flags (atomics only) into
// the pure HangDetector: the 30 s boot budget applies until Arm(); then 5
// consecutive 1 s periods without progress, with no excuse and no debugger,
// are a hang (ADR-104). A hang gets one out-of-process dump per episode,
// written on this thread with DumpWithoutCrash, its reason labelled a GPU or
// compositor stall when main was inside an SDL_GPU or pacer wait; the
// episode's end, when main makes progress again, is logged with its length.
// The watchdog never calls DbgHelp, never breaks into a debugger and never
// ends the process for a hang. Once the shutdown deadline starts (5 s after
// the quit drain) hangs are no longer judged; when it expires the watchdog
// requests a dump, has the log worker flush and ends the process.

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
// iterate counter, relaxed; the Suspended, Modal, heartbeat and in-driver
// flags, release stores). Each must outlive the watchdog; a null one reads
// as 0 or false.
struct WatchdogInputs {
  const std::atomic<std::uint64_t>* iterate_counter = nullptr;
  const std::atomic<bool>* modal = nullptr;
  const std::atomic<bool>* suspended = nullptr;
  const std::atomic<bool>* heartbeat = nullptr;
  // Main is inside an SDL_GPU or pacer wait (se_platform/driver_wait.h).
  const std::atomic<bool>* in_driver = nullptr;
};

// Writes one dump; DumpWithoutCrash unless a test substitutes its own.
using DumpFunction = std::expected<void, Error> (*)(std::string_view reason);
// The unbiased clock and the debugger check, which tests substitute.
using ClockFunction = std::uint64_t (*)();
using DebuggerFunction = bool (*)();

// The exit code of a process the shutdown deadline ended.
inline constexpr int kShutdownDeadlineExitCode = 3;
// Dump requests that may wait for the watchdog thread.
inline constexpr std::uint32_t kDumpRequestQueue = 8;
// The longest reason a request keeps.
inline constexpr std::size_t kDumpRequestReasonSize = 120;

struct WatchdogConfig {
  WatchdogInputs inputs{};
  // The hang period, on |clock|; detector.period_ns is set from it.
  std::uint32_t period_ms = 1000;
  // The real time between samples; 0: a quarter period (at least 1 ms), so
  // a hang is declared within a quarter period of the fifth period's end
  // (ADR-104).
  std::uint32_t sample_ms = 0;
  HangDetectorConfig detector{};
  // When the boot started (on |clock|); 0: when the watchdog starts.
  std::uint64_t boot_start_ns = 0;
  // From StartShutdownDeadline() to the dump, the log flush and the end of
  // the process.
  std::uint64_t shutdown_deadline_ns = 5'000'000'000;
  // End the process when the deadline expires (tests turn it off and read
  // Stats().deadline_expired).
  bool terminate_on_deadline = true;
  // nullptr: DumpWithoutCrash.
  DumpFunction dump = nullptr;
  // nullptr: se::UnbiasedNowNs. A test's fake clock decides the periods;
  // the waits between samples stay real.
  ClockFunction clock = nullptr;
  // nullptr: se::DebuggerAttached.
  DebuggerFunction debugger = nullptr;
};

struct WatchdogStats {
  // Wakes that sampled main's state (every wake but the last).
  std::uint64_t samples = 0;
  // Whole periods without progress that nothing excused.
  std::uint64_t stalled_periods = 0;
  std::uint64_t dumps_written = 0;
  std::uint64_t dumps_failed = 0;
  // RequestDump calls refused because the queue was full.
  std::uint64_t requests_dropped = 0;
  // Hangs declared, hang dumps written (each also counts in dumps_written
  // or dumps_failed), and episodes that ended with main's progress.
  std::uint64_t hangs = 0;
  std::uint64_t hang_dumps_written = 0;
  std::uint64_t hangs_ended = 0;
  // The last ended episode's length.
  std::uint64_t last_episode_ns = 0;
  bool armed = false;
  bool boot_over_budget = false;
  // A hang is declared and main has not made progress since.
  bool in_hang = false;
  // The last hang was labelled a GPU or compositor stall.
  bool last_hang_in_driver = false;
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

  // Shutdown step 3: the quit drain is over and the deadline starts; hangs
  // are no longer judged.
  void StartShutdownDeadline() noexcept;

  // Requests a stop, serves the dump requests still queued and joins.
  // Idempotent.
  void Stop() noexcept;

  [[nodiscard]] bool Running() const noexcept;
  // Any thread.
  [[nodiscard]] WatchdogStats Stats() const noexcept;

 private:
  struct State;
  explicit Watchdog(std::unique_ptr<State> state) noexcept;

  std::unique_ptr<State> state_;
};

}  // namespace se::crash

#endif  // SE_CRASH_WATCHDOG_H_
