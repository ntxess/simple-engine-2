// Copyright 2026 The simple-engine-2 Authors.

#include "se_os/precise_wait.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>

#include "se_core/error.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/prctl.h>
#include <time.h>
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || \
    defined(__i386__)
#include <immintrin.h>
#endif

namespace se {

#if !defined(_WIN32)
namespace {

constexpr std::uint64_t kNsPerSecond = 1'000'000'000;

}  // namespace
#endif

std::uint64_t MonotonicNowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void CpuRelax() noexcept {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || \
    defined(__i386__)
  _mm_pause();
#endif
}

std::expected<PreciseTimer, Error> PreciseTimer::Create() {
#if defined(_WIN32)
  HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
  bool high_res = handle != nullptr;
  if (handle == nullptr) {
    // Before Windows 10 1803 the flag is rejected.
    handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }
  if (handle == nullptr) {
    return Fail(ErrorCode::kUnsupported,
                "PreciseTimer: CreateWaitableTimerExW failed (error " +
                    std::to_string(GetLastError()) + ")");
  }
  return PreciseTimer(handle, high_res);
#else
  // Best effort: the default slack (50 us) only makes the wake later.
  // prctl takes unsigned longs: the slack is 1000 ns.
  static_cast<void>(prctl(PR_SET_TIMERSLACK, 1000UL, 0UL, 0UL, 0UL));
  return PreciseTimer(nullptr, true);
#endif
}

PreciseTimer::PreciseTimer(PreciseTimer&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      high_res_(other.high_res_) {}

PreciseTimer& PreciseTimer::operator=(PreciseTimer&& other) noexcept {
  if (this != &other) {
#if defined(_WIN32)
    if (handle_ != nullptr) CloseHandle(handle_);
#endif
    handle_ = std::exchange(other.handle_, nullptr);
    high_res_ = other.high_res_;
  }
  return *this;
}

PreciseTimer::~PreciseTimer() {
#if defined(_WIN32)
  if (handle_ != nullptr) CloseHandle(handle_);
#endif
}

void PreciseTimer::SleepUntil(std::uint64_t deadline_ns) noexcept {
  const std::uint64_t now = MonotonicNowNs();
  if (now >= deadline_ns) return;
#if defined(_WIN32)
  // Relative due time in 100 ns units, negative.
  LARGE_INTEGER due;
  due.QuadPart = -static_cast<LONGLONG>((deadline_ns - now + 99) / 100);
  if (handle_ != nullptr &&
      SetWaitableTimerEx(handle_, &due, 0, nullptr, nullptr, nullptr, 0) != 0) {
    WaitForSingleObject(handle_, INFINITE);
    return;
  }
  // No timer: Sleep's millisecond granularity, rounded down so the caller's
  // spin takes the rest.
  const auto ms = static_cast<DWORD>((deadline_ns - now) / 1'000'000);
  if (ms > 0) Sleep(ms);
#else
  timespec at{};
  at.tv_sec = static_cast<time_t>(deadline_ns / kNsPerSecond);
  at.tv_nsec = static_cast<decltype(at.tv_nsec)>(deadline_ns % kNsPerSecond);
  // EINTR (a signal) returns early; the caller re-checks the time.
  static_cast<void>(
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, nullptr));
#endif
}

}  // namespace se
