// Copyright 2026 The simple-engine-2 Authors.
//
// Precise absolute waits for the dedicated sim thread's PacedClock (docs/
// design/dedicated-driver.md "Threads", sim, and "Platform Fit" 11; ADR-084).
//
//   MonotonicNowNs()  the engine's one clock, std::chrono::steady_clock in
//                     nanoseconds: QueryPerformanceCounter on MSVC,
//                     CLOCK_MONOTONIC on libstdc++
//   PreciseTimer      sleeps to an absolute time on that clock: a waitable
//                     timer created with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
//                     on Windows (relative due times, since waitable timers'
//                     absolute times are UTC; a plain waitable timer where
//                     the high-resolution flag is not supported), and
//                     clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) on Linux
//                     with the calling thread's timer slack lowered to 1 us
//                     (PR_SET_TIMERSLACK, for the thread that creates it)
//   CpuRelax()        one spin-wait hint: the pause instruction
//
// The timer resolution is never toggled (no timeBeginPeriod). A timer wakes
// late by the OS's granularity and may wake a little early on Windows, so
// the caller wakes it before the deadline and spins the rest.

#ifndef SE_OS_PRECISE_WAIT_H_
#define SE_OS_PRECISE_WAIT_H_

#include <cstdint>
#include <expected>

#include "se_core/error.h"

namespace se {

[[nodiscard]] std::uint64_t MonotonicNowNs() noexcept;

void CpuRelax() noexcept;

class PreciseTimer {
 public:
  // A timer for the calling thread.
  [[nodiscard]] static std::expected<PreciseTimer, Error> Create();

  PreciseTimer(PreciseTimer&& other) noexcept;
  PreciseTimer& operator=(PreciseTimer&& other) noexcept;
  PreciseTimer(const PreciseTimer&) = delete;
  PreciseTimer& operator=(const PreciseTimer&) = delete;
  ~PreciseTimer();

  // Returns at or after |deadline_ns| on MonotonicNowNs's clock (see the
  // file comment for early wakes), at once when it has passed.
  void SleepUntil(std::uint64_t deadline_ns) noexcept;

  // Windows: the high-resolution flag took. Linux: always.
  [[nodiscard]] bool HighResolution() const noexcept { return high_res_; }

 private:
  PreciseTimer(void* handle, bool high_res) noexcept
      : handle_(handle), high_res_(high_res) {}

  // The waitable timer (Windows); unused on Linux.
  void* handle_ = nullptr;
  bool high_res_ = false;
};

}  // namespace se

#endif  // SE_OS_PRECISE_WAIT_H_
