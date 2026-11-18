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
#include <format>
#include <memory>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/breadcrumb.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/profile.h"
#include "se_core/schedule.h"
#include "se_core/spsc_ring.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_crash/crash.h"
#include "se_crash/hang_detector.h"
#include "se_os/debugger.h"
#include "se_os/thread.h"
#include "se_os/unbiased_clock.h"

namespace se::crash {
namespace {

constexpr std::uint64_t kNsPerMs = 1'000'000;
constexpr double kNsToSec = 1e-9;
// How long the deadline path waits for the log worker to flush.
constexpr std::uint64_t kDeadlineFlushNs = 1'000'000'000;
// How long the watchdog waits, after a hang's dump, for the log worker to
// write the records up to it (the hang's own line and the dump's
// included). After the dump: the wait takes nothing from the 6 s between a
// hang's start and its dump.
constexpr std::uint64_t kHangFlushNs = 200'000'000;

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

std::uint64_t RealClock() { return UnbiasedNowNs(); }

// The real time between samples: sample_ms, or a quarter period.
std::uint64_t SampleNs(const WatchdogConfig& config) noexcept {
  const std::uint64_t ns = config.sample_ms != 0
                               ? std::uint64_t{config.sample_ms} * kNsPerMs
                               : std::uint64_t{config.period_ms} * kNsPerMs / 4;
  return std::max(ns, kNsPerMs);
}
bool RealDebugger() { return DebuggerAttached(); }

// Main's breadcrumb (ThreadRegistry slot 0), for the hang's log line; zero
// when main has no live slot.
BreadcrumbFields MainBreadcrumb() noexcept {
  const ThreadSlotInfo main = ThreadRegistry::Instance().Read(kMainThreadSlot);
  if (main.state != ThreadSlotState::kLive || main.role != ThreadRole::kMain) {
    return {};
  }
  return UnpackBreadcrumb(main.breadcrumb);
}

std::string_view BreadcrumbPhase(const BreadcrumbFields& crumb) noexcept {
  if (crumb.phase >= kPhaseCount) return "?";
  return PhaseName(static_cast<Phase>(crumb.phase));
}

}  // namespace

struct Watchdog::State {
  explicit State(const WatchdogConfig& watchdog_config)
      : config(watchdog_config),
        dump(watchdog_config.dump != nullptr ? watchdog_config.dump
                                             : &DumpWithoutCrash),
        clock(watchdog_config.clock != nullptr ? watchdog_config.clock
                                               : &RealClock),
        debugger(watchdog_config.debugger != nullptr ? watchdog_config.debugger
                                                     : &RealDebugger),
        sample_ns(SampleNs(watchdog_config)),
        requests(kDumpRequestQueue) {
    config.detector.period_ns = std::uint64_t{config.period_ms} * kNsPerMs;
  }

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
  void OnHang(const HangDetector& detector, const HangSample& sample);
  void OnHangEnded(const HangDetector& detector);
  void CheckDeadline(std::uint64_t now_ns);
  bool WriteDump(std::string_view reason);
  [[nodiscard]] std::uint64_t Iterates() const noexcept {
    return config.inputs.iterate_counter != nullptr
               ? config.inputs.iterate_counter->load(std::memory_order_relaxed)
               : 0;
  }
  [[nodiscard]] std::uint64_t WaitNs(std::uint64_t now_ns) const noexcept;

  WatchdogConfig config;
  DumpFunction dump;
  ClockFunction clock;
  DebuggerFunction debugger;
  std::uint64_t sample_ns = 0;
  // Main produces, the watchdog consumes.
  SpscRing<DumpRequest> requests;
  std::binary_semaphore wake{0};
  std::atomic<bool> wake_pending{false};
  std::atomic<bool> armed{false};
  std::atomic<bool> stopped{false};
  // clock() when the deadline started; 0 while it has not.
  std::atomic<std::uint64_t> deadline_start_ns{0};
  // The watchdog thread's own: the counter at the last sample, the stalled
  // periods counted into the stats, whether the episode's dump was written.
  std::uint64_t last_iterates = 0;
  std::uint32_t counted_periods = 0;
  bool hang_dump_ok = false;
  std::atomic<std::uint64_t> samples{0};
  std::atomic<std::uint64_t> stalled_periods{0};
  std::atomic<std::uint64_t> dumps_written{0};
  std::atomic<std::uint64_t> dumps_failed{0};
  std::atomic<std::uint64_t> requests_dropped{0};
  std::atomic<std::uint64_t> hangs{0};
  std::atomic<std::uint64_t> hang_dumps_written{0};
  std::atomic<std::uint64_t> hangs_ended{0};
  std::atomic<std::uint64_t> last_episode_ns{0};
  std::atomic<std::uint64_t> unobserved_gaps{0};
  std::atomic<bool> detector_armed{false};
  std::atomic<bool> boot_over_budget{false};
  std::atomic<bool> in_hang{false};
  std::atomic<bool> last_hang_in_driver{false};
  std::atomic<bool> deadline_expired{false};
  Thread thread;
};

void Watchdog::State::Run(const std::stop_token& stop) {
  const std::stop_callback wake_on_stop(stop, [this] { Wake(); });
  HangDetector detector(config.detector);
  const std::uint64_t started = clock();
  detector.Start(config.boot_start_ns != 0 ? config.boot_start_ns : started);
  for (;;) {
    const std::uint64_t wait_ns = WaitNs(clock());
    if (wake.try_acquire_for(std::chrono::nanoseconds(wait_ns))) {
      wake_pending.exchange(false, std::memory_order_acq_rel);
    }
    // A zone per wake, so the thread shows in a capture under its name.
    SE_PROFILE_ZONE("Watchdog wake");
    ServeRequests();
    if (stop.stop_requested()) break;
    const std::uint64_t now = clock();
    if (!detector.Armed() && armed.load(std::memory_order_acquire)) {
      last_iterates = Iterates();
      detector.Arm(last_iterates, now);
      detector_armed.store(true, std::memory_order_release);
    }
    // The shutdown deadline owns the end of the process (ADR-105).
    if (!detector.Disarmed() &&
        deadline_start_ns.load(std::memory_order_acquire) != 0) {
      detector.Disarm();
    }
    Judge(detector, now);
    CheckDeadline(now);
    samples.fetch_add(1, std::memory_order_release);
  }
  // Requests queued before the stop are still written.
  ServeRequests();
}

std::uint64_t Watchdog::State::WaitNs(std::uint64_t now_ns) const noexcept {
  std::uint64_t wait = sample_ns;
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
  if (detector.Disarmed()) return;
  HangSample sample;
  sample.now_ns = now_ns;
  sample.iterates = Iterates();
  const bool moved = sample.iterates != last_iterates;
  last_iterates = sample.iterates;
  sample.suspended = Load(config.inputs.suspended);
  sample.modal = Load(config.inputs.modal);
  sample.heartbeat = Load(config.inputs.heartbeat);
  sample.in_driver = Load(config.inputs.in_driver);
  // Linux reads /proc for it: only when it can matter.
  sample.debugger = !moved && debugger();
  const HangVerdict verdict = detector.Observe(sample);
  const std::uint32_t whole = detector.StalledPeriods();
  if (whole > counted_periods) {
    stalled_periods.fetch_add(whole - counted_periods,
                              std::memory_order_relaxed);
  }
  counted_periods = whole;
  switch (verdict) {
    case HangVerdict::kProgress:
    case HangVerdict::kExcused:
    case HangVerdict::kStalled:
    case HangVerdict::kOff:
      break;
    case HangVerdict::kUnobserved:
      unobserved_gaps.fetch_add(1, std::memory_order_release);
      SE_LOG_WARN(
          "the watchdog took no sample for {:.1f} s (the process was "
          "stopped: a debugger, a job-control stop or a paused machine): "
          "that time is not counted towards a hang",
          static_cast<double>(detector.GapNs()) * kNsToSec);
      break;
    case HangVerdict::kBootOverBudget:
      boot_over_budget.store(true, std::memory_order_release);
      SE_LOG_ERROR("the boot has taken more than {} s: requesting a dump",
                   config.detector.boot_budget_ns / 1'000'000'000);
      WriteDump("boot over budget");
      break;
    case HangVerdict::kHang:
      OnHang(detector, sample);
      break;
    case HangVerdict::kHangEnded:
      OnHangEnded(detector);
      break;
  }
}

void Watchdog::State::OnHang(const HangDetector& detector,
                             const HangSample& sample) {
  const std::uint64_t number = detector.Hangs();
  const bool in_driver = detector.HangInDriver();
  hangs.store(number, std::memory_order_release);
  last_hang_in_driver.store(in_driver, std::memory_order_release);
  in_hang.store(true, std::memory_order_release);
  const BreadcrumbFields crumb = MainBreadcrumb();
  const double stall_sec = static_cast<double>(detector.StallNs()) * kNsToSec;
  // The reason is the dump's se.dump_reason annotation: the label goes into
  // the dump as well as the log (kDumpRequestReasonSize bytes are kept by
  // RequestDump; this one is passed whole).
  const std::string reason =
      in_driver ? std::format(
                      "hang {}: GPU or compositor stall, main in an SDL_GPU "
                      "or pacer wait for {:.1f} s at tick {}",
                      number, stall_sec, crumb.tick)
                : std::format(
                      "hang {}: main made no progress for {:.1f} s "
                      "at tick {}",
                      number, stall_sec, crumb.tick);
  SE_LOG_ERROR(
      "{} (iterate {}; main's breadcrumb: tick {}, phase {}, system {}, "
      "chunk {}): requesting a dump",
      reason, sample.iterates, crumb.tick, BreadcrumbPhase(crumb), crumb.system,
      crumb.chunk);
  hang_dump_ok = WriteDump(reason);
  if (hang_dump_ok) {
    hang_dumps_written.fetch_add(1, std::memory_order_release);
  }
  // The log file is complete up to the hang and its dump (mt-design.md
  // P12: a hung process is often ended by hand next); the rings are dump
  // streams either way.
  static_cast<void>(FlushLog(kHangFlushNs));
}

void Watchdog::State::OnHangEnded(const HangDetector& detector) {
  const std::uint64_t episode = detector.EpisodeNs();
  last_episode_ns.store(episode, std::memory_order_relaxed);
  hangs_ended.fetch_add(1, std::memory_order_release);
  in_hang.store(false, std::memory_order_release);
  SE_LOG_WARN(
      "hang {} ended: main made progress again after {:.2f} s (Hung -> "
      "Running){}",
      detector.Hangs(), static_cast<double>(episode) * kNsToSec,
      hang_dump_ok ? "" : "; its dump was not written");
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

bool Watchdog::State::WriteDump(std::string_view reason) {
  const std::expected<void, Error> written = dump(reason);
  if (written) {
    dumps_written.fetch_add(1, std::memory_order_release);
    SE_LOG_INFO("dump written ({})", reason);
    return true;
  }
  dumps_failed.fetch_add(1, std::memory_order_release);
  SE_LOG_WARN("dump not written ({}): {}", reason, written.error().message);
  return false;
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
          expected, std::max<std::uint64_t>(state_->clock(), 1),
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
  stats.samples = s.samples.load(std::memory_order_acquire);
  stats.stalled_periods = s.stalled_periods.load(std::memory_order_relaxed);
  stats.dumps_written = s.dumps_written.load(std::memory_order_acquire);
  stats.dumps_failed = s.dumps_failed.load(std::memory_order_acquire);
  stats.requests_dropped = s.requests_dropped.load(std::memory_order_relaxed);
  stats.hangs = s.hangs.load(std::memory_order_acquire);
  stats.hang_dumps_written =
      s.hang_dumps_written.load(std::memory_order_acquire);
  stats.hangs_ended = s.hangs_ended.load(std::memory_order_acquire);
  stats.last_episode_ns = s.last_episode_ns.load(std::memory_order_relaxed);
  stats.unobserved_gaps = s.unobserved_gaps.load(std::memory_order_acquire);
  stats.armed = s.detector_armed.load(std::memory_order_acquire);
  stats.boot_over_budget = s.boot_over_budget.load(std::memory_order_acquire);
  stats.in_hang = s.in_hang.load(std::memory_order_acquire);
  stats.last_hang_in_driver =
      s.last_hang_in_driver.load(std::memory_order_acquire);
  stats.deadline_started =
      s.deadline_start_ns.load(std::memory_order_acquire) != 0;
  stats.deadline_expired = s.deadline_expired.load(std::memory_order_acquire);
  return stats;
}

}  // namespace se::crash
