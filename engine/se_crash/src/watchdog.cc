// Copyright 2026 The simple-engine-2 Authors.

#include "se_crash/watchdog.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <string_view>
#include <utility>

#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/profile.h"
#include "se_core/spsc_ring.h"
#include "se_core/thread_role.h"
#include "se_crash/crash.h"
#include "se_crash/hang_detector.h"
#include "se_os/debugger.h"
#include "se_os/thread.h"
#include "se_os/unbiased_clock.h"

namespace se::crash {
namespace {

constexpr std::uint64_t kNsPerMs = 1'000'000;
// How long the deadline path waits for the log worker to flush.
constexpr std::uint64_t kDeadlineFlushNs = 1'000'000'000;

struct DumpRequest {
  std::array<char, kDumpRequestReasonSize> reason{};
  std::uint32_t length = 0;

  [[nodiscard]] std::string_view Reason() const noexcept {
    return {reason.data(), length};
  }
};

// Ends the process at once: no static destructors or DLL teardown, which
// have no deadline of their own.
[[noreturn]] void EndProcess(int exit_code) noexcept {
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exit_code));
#endif
  std::_Exit(exit_code);
}

bool Load(const std::atomic<bool>* flag) noexcept {
  return flag != nullptr && flag->load(std::memory_order_acquire);
}

}  // namespace

struct Watchdog::State {
  explicit State(const WatchdogConfig& watchdog_config)
      : config(watchdog_config),
        dump(watchdog_config.dump != nullptr ? watchdog_config.dump
                                             : &DumpWithoutCrash),
        requests(kDumpRequestQueue) {}

  void Run(const std::stop_token& stop);
  // Releases the semaphore once per wait (a binary_semaphore must not be
  // released past 1).
  void Wake() noexcept {
    if (!wake_pending.exchange(true, std::memory_order_acq_rel)) {
      wake.release();
    }
  }
  void ServeRequests();
  void Judge(HangDetector& detector, std::uint64_t now_ns);
  void CheckDeadline(std::uint64_t now_ns);
  void WriteDump(std::string_view reason);
  [[nodiscard]] std::uint64_t WaitNs(std::uint64_t now_ns) const noexcept;

  WatchdogConfig config;
  DumpFunction dump;
  // Main produces, the watchdog consumes.
  SpscRing<DumpRequest> requests;
  std::binary_semaphore wake{0};
  std::atomic<bool> wake_pending{false};
  std::atomic<bool> armed{false};
  std::atomic<bool> stopped{false};
  // UnbiasedNowNs() when the deadline started; 0 while it has not.
  std::atomic<std::uint64_t> deadline_start_ns{0};
  std::uint64_t last_period_ns = 0;
  std::atomic<std::uint64_t> periods{0};
  std::atomic<std::uint64_t> stalled_periods{0};
  std::atomic<std::uint64_t> dumps_written{0};
  std::atomic<std::uint64_t> dumps_failed{0};
  std::atomic<std::uint64_t> requests_dropped{0};
  std::atomic<bool> detector_armed{false};
  std::atomic<bool> boot_over_budget{false};
  std::atomic<bool> deadline_expired{false};
  Thread thread;
};

void Watchdog::State::Run(const std::stop_token& stop) {
  const std::stop_callback wake_on_stop(stop, [this] { Wake(); });
  HangDetector detector(config.detector);
  const std::uint64_t started = UnbiasedNowNs();
  detector.Start(config.boot_start_ns != 0 ? config.boot_start_ns : started);
  last_period_ns = started;
  for (;;) {
    const std::uint64_t wait_ns = WaitNs(UnbiasedNowNs());
    if (wake.try_acquire_for(std::chrono::nanoseconds(wait_ns))) {
      wake_pending.exchange(false, std::memory_order_acq_rel);
    }
    // A zone per wake, so the thread shows in a capture under its name.
    SE_PROFILE_ZONE("Watchdog wake");
    ServeRequests();
    if (stop.stop_requested()) break;
    const std::uint64_t now = UnbiasedNowNs();
    if (!detector.Armed() && armed.load(std::memory_order_acquire)) {
      detector.Arm(
          config.inputs.iterate_counter != nullptr
              ? config.inputs.iterate_counter->load(std::memory_order_relaxed)
              : 0);
      detector_armed.store(true, std::memory_order_release);
    }
    Judge(detector, now);
    CheckDeadline(now);
  }
  // Requests queued before the stop are still written.
  ServeRequests();
}

std::uint64_t Watchdog::State::WaitNs(std::uint64_t now_ns) const noexcept {
  const std::uint64_t period_ns = std::uint64_t{config.period_ms} * kNsPerMs;
  std::uint64_t wait = period_ns;
  if (const std::uint64_t start =
          deadline_start_ns.load(std::memory_order_acquire);
      start != 0 && !deadline_expired.load(std::memory_order_relaxed)) {
    const std::uint64_t end = start + config.shutdown_deadline_ns;
    wait = std::min(wait, end > now_ns ? end - now_ns : 0);
  }
  return std::max<std::uint64_t>(wait, kNsPerMs);
}

void Watchdog::State::ServeRequests() {
  while (std::optional<DumpRequest> request = requests.TryPop()) {
    WriteDump(request->Reason());
  }
}

void Watchdog::State::Judge(HangDetector& detector, std::uint64_t now_ns) {
  const std::uint64_t period_ns = std::uint64_t{config.period_ms} * kNsPerMs;
  if (now_ns - last_period_ns < period_ns) return;
  last_period_ns = now_ns;
  HangSample sample;
  sample.now_ns = now_ns;
  sample.iterates =
      config.inputs.iterate_counter != nullptr
          ? config.inputs.iterate_counter->load(std::memory_order_relaxed)
          : 0;
  sample.suspended = Load(config.inputs.suspended);
  sample.modal = Load(config.inputs.modal);
  sample.heartbeat = Load(config.inputs.heartbeat);
  sample.debugger = DebuggerAttached();
  periods.fetch_add(1, std::memory_order_relaxed);
  switch (detector.Observe(sample)) {
    case HangVerdict::kProgress:
    case HangVerdict::kExcused:
      break;
    case HangVerdict::kStalled:
      stalled_periods.fetch_add(1, std::memory_order_relaxed);
      break;
    case HangVerdict::kBootOverBudget:
      boot_over_budget.store(true, std::memory_order_release);
      SE_LOG_ERROR("the boot has taken more than {} s: requesting a dump",
                   config.detector.boot_budget_ns / 1'000'000'000);
      WriteDump("boot over budget");
      break;
    case HangVerdict::kHang:
      stalled_periods.fetch_add(1, std::memory_order_relaxed);
      SE_LOG_ERROR("main made no progress for {} periods: requesting a dump",
                   detector.StalledPeriods());
      WriteDump("hang");
      break;
  }
}

void Watchdog::State::CheckDeadline(std::uint64_t now_ns) {
  const std::uint64_t start = deadline_start_ns.load(std::memory_order_acquire);
  if (start == 0 || deadline_expired.load(std::memory_order_relaxed)) return;
  if (now_ns < start + config.shutdown_deadline_ns) return;
  deadline_expired.store(true, std::memory_order_release);
  SE_LOG_ERROR(
      "shutdown has not finished {} ms after the quit drain: requesting a "
      "dump and ending the process",
      config.shutdown_deadline_ns / kNsPerMs);
  WriteDump("shutdown deadline");
  static_cast<void>(FlushLog(kDeadlineFlushNs));
  if (config.terminate_on_deadline) EndProcess(kShutdownDeadlineExitCode);
}

void Watchdog::State::WriteDump(std::string_view reason) {
  if (auto written = dump(reason); written) {
    dumps_written.fetch_add(1, std::memory_order_release);
    SE_LOG_INFO("dump written ({})", reason);
  } else {
    dumps_failed.fetch_add(1, std::memory_order_release);
    SE_LOG_WARN("dump not written ({}): {}", reason, written.error().message);
  }
}

std::expected<Watchdog, Error> Watchdog::Start(const WatchdogConfig& config) {
  auto state = std::make_unique<State>(config);
  State* raw = state.get();
  auto thread =
      StartThread({.role = ThreadRole::kWatchdog, .name = "watchdog"},
                  [raw](const std::stop_token& stop) { raw->Run(stop); });
  if (!thread) return std::unexpected(std::move(thread.error()));
  state->thread = std::move(*thread);
  return Watchdog(std::move(state));
}

Watchdog::Watchdog(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

Watchdog::Watchdog(Watchdog&& other) noexcept = default;

Watchdog& Watchdog::operator=(Watchdog&& other) noexcept {
  if (this != &other) {
    Stop();
    state_ = std::move(other.state_);
  }
  return *this;
}

Watchdog::~Watchdog() { Stop(); }

void Watchdog::Arm() noexcept {
  if (state_) state_->armed.store(true, std::memory_order_release);
}

bool Watchdog::RequestDump(std::string_view reason) noexcept {
  if (!state_ || state_->stopped.load(std::memory_order_acquire)) return false;
  DumpRequest request;
  request.length = static_cast<std::uint32_t>(
      std::min(reason.size(), request.reason.size()));
  std::memcpy(request.reason.data(), reason.data(), request.length);
  if (!state_->requests.TryPush(request)) {
    state_->requests_dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  state_->Wake();
  return true;
}

void Watchdog::StartShutdownDeadline() noexcept {
  if (!state_) return;
  std::uint64_t expected = 0;
  if (state_->deadline_start_ns.compare_exchange_strong(
          expected, std::max<std::uint64_t>(UnbiasedNowNs(), 1),
          std::memory_order_acq_rel)) {
    state_->Wake();
  }
}

void Watchdog::Stop() noexcept {
  if (!state_ || state_->stopped.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  state_->thread.RequestStop();
  state_->thread.Join();
}

bool Watchdog::Running() const noexcept {
  return state_ && !state_->stopped.load(std::memory_order_acquire);
}

WatchdogStats Watchdog::Stats() const noexcept {
  WatchdogStats stats;
  if (!state_) return stats;
  const State& s = *state_;
  stats.periods = s.periods.load(std::memory_order_relaxed);
  stats.stalled_periods = s.stalled_periods.load(std::memory_order_relaxed);
  stats.dumps_written = s.dumps_written.load(std::memory_order_acquire);
  stats.dumps_failed = s.dumps_failed.load(std::memory_order_acquire);
  stats.requests_dropped = s.requests_dropped.load(std::memory_order_relaxed);
  stats.armed = s.detector_armed.load(std::memory_order_acquire);
  stats.boot_over_budget = s.boot_over_budget.load(std::memory_order_acquire);
  stats.deadline_started =
      s.deadline_start_ns.load(std::memory_order_acquire) != 0;
  stats.deadline_expired = s.deadline_expired.load(std::memory_order_acquire);
  return stats;
}

}  // namespace se::crash
