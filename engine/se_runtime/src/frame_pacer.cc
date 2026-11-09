// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/frame_pacer.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/assert.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_core/time.h"
#include "se_platform/vblank_sampler.h"
#include "se_runtime/pacer_policy.h"
#include "se_runtime/present_port.h"

namespace se {
namespace {

constexpr double kNsPerSec = 1e9;
constexpr std::uint64_t kLogIntervalNs = 1'000'000'000;
// VRR aims early by the timer's median lateness, at most this much.
constexpr double kMaxVrrLeadSec = 1.0e-3;
// Lateness samples before the measured timer p99 replaces the default.
constexpr std::size_t kMinLatenessSamples = 16;

class SdlPacerClock final : public PacerClock {
 public:
  std::uint64_t NowNs() noexcept override { return SDL_GetTicksNS(); }
  void SleepNs(std::uint64_t ns) noexcept override { SDL_DelayNS(ns); }
};

std::uint64_t ToNs(double sec) noexcept {
  return static_cast<std::uint64_t>(
      std::llround(std::max(0.0, sec) * kNsPerSec));
}

double ToSec(std::uint64_t ns) noexcept {
  return static_cast<double>(ns) / kNsPerSec;
}

// Presentation intervals between two frame starts, rounded.
std::uint32_t Intervals(std::uint64_t start, std::uint64_t prev,
                        double period_ns) noexcept {
  if (prev == 0) return 1;
  if (start <= prev || !(period_ns > 0.0)) return 0;
  return static_cast<std::uint32_t>(
      std::llround(static_cast<double>(start - prev) / period_ns));
}

}  // namespace

PacerClock& SdlClock() {
  static auto* const kClock = new SdlPacerClock();
  return *kClock;
}

FramePacer::FramePacer(const FramePacerConfig& config, PacerClock* clock)
    : config_(config),
      clock_(clock != nullptr ? clock : &SdlClock()),
      latency_(config.frame_latency, config.adaptive_latency) {}

void FramePacer::Attach(PresentPort* port, VblankReader sampler) {
  AssertMainThread();
  port_ = port;
  sampler_ = std::move(sampler);
  caps_ = port_ != nullptr ? port_->QueryCaps() : PresentCaps{};
  tracker_.Reset();
  wait_tracker_.Reset();
  stats_live_ = false;
  carried_ = WaitTracker::Result{};
  half_rate_.Reset();
  latency_.Reset();
  target_ns_ = 0;
  vrr_deadline_ns_ = 0;
  last_present_id_ = 0;
  paced_since_ns_ = 0;
  if (port_ != nullptr && caps_.frame_latency_waitable) {
    if (!port_->SetFrameLatency(latency_.Latency())) {
      SE_LOG_WARN("pacing: frame latency {} could not be set",
                  latency_.Latency());
    }
  }
  const std::uint64_t now = clock_->NowNs();
  Evaluate(now, /*initial=*/true);
  next_evaluation_ns_ = now + ToNs(kReevaluateSec);
}

void FramePacer::Detach() {
  AssertMainThread();
  if (port_ != nullptr && mode_ == PacingMode::kVrr) {
    static_cast<void>(port_->SetPresentMode(SwapchainMode::kVsync));
  }
  port_ = nullptr;
  sampler_ = {};
  caps_ = PresentCaps{};
  mode_ = PacingMode::kStandard;
  mode_reason_ = "detached";
}

void FramePacer::SetDisplayRefresh(double hz) {
  AssertMainThread();
  display_hz_ = hz > 0.0 ? hz : 0.0;
  next_evaluation_ns_ = 0;
}

void FramePacer::OnOutputChanged() {
  AssertMainThread();
  target_ns_ = 0;
  last_present_id_ = 0;
  paced_since_ns_ = 0;
  tracker_.Reset();
  wait_tracker_.Reset();
  next_evaluation_ns_ = 0;
}

void FramePacer::ResetClock() noexcept {
  prev_start_ns_ = 0;
  prev_now_ns_ = 0;
  measured_vblank_sec_ = 0.0;
  target_ns_ = 0;
  vrr_deadline_ns_ = 0;
  tracker_.Reset();
  wait_tracker_.Reset();
}

int FramePacer::PresentInterval() const noexcept {
  return mode_ == PacingMode::kVrr ? 1 : half_rate_.PresentInterval();
}

double FramePacer::TimerP99Sec() const noexcept {
  if (lateness_.Count() < kMinLatenessSamples) return kDefaultTimerP99Sec;
  return lateness_.Percentile(0.99, kDefaultTimerP99Sec);
}

SamplerSample FramePacer::Sample(const VblankReading& reading) const {
  SamplerSample sample;
  sample.valid = reading.valid;
  sample.period_sec = ToSec(reading.period_ns);
  sample.spread_sec = ToSec(reading.spread_ns);
  return sample;
}

void FramePacer::Evaluate(std::uint64_t now_ns, bool initial) {
  if (port_ == nullptr) {
    if (initial || mode_ != PacingMode::kStandard) {
      SetMode(PacingMode::kStandard, "no presentation (headless)");
    }
    return;
  }
  const VblankReading reading = sampler_ ? sampler_(now_ns) : VblankReading{};
  ModeInputs inputs;
  inputs.request = config_.request;
  inputs.paced_by_default = config_.paced_by_default;
  inputs.caps = caps_;
  inputs.sampler = Sample(reading);
  inputs.display_period_sec = display_hz_ > 0.0 ? 1.0 / display_hz_ : 0.0;
  const ModeChoice choice = ChooseMode(inputs);
  if (choice.mode == PacingMode::kPaced && mode_ != PacingMode::kPaced &&
      !initial) {
    // Promote only after the sampler held for kPromoteSec.
    if (paced_since_ns_ == 0) {
      paced_since_ns_ = now_ns;
      return;
    }
    if (now_ns - paced_since_ns_ < ToNs(kPromoteSec)) return;
  }
  if (choice.mode != PacingMode::kPaced) paced_since_ns_ = 0;
  if (initial || choice.mode != mode_ || choice.reason != mode_reason_) {
    SetMode(choice.mode, choice.reason);
  }
}

void FramePacer::SetMode(PacingMode mode, std::string_view reason) {
  const PacingMode old = mode_;
  if (port_ != nullptr && mode == PacingMode::kVrr && old != PacingMode::kVrr &&
      !port_->SetPresentMode(SwapchainMode::kImmediate)) {
    mode = PacingMode::kStandard;
    reason = "--pacing vrr, but the swapchain refused IMMEDIATE";
  }
  if (port_ != nullptr && mode != PacingMode::kVrr && old == PacingMode::kVrr) {
    static_cast<void>(port_->SetPresentMode(SwapchainMode::kVsync));
  }
  if (mode != old) {
    ++totals_.mode_changes;
    target_ns_ = 0;
    vrr_deadline_ns_ = 0;
    last_present_id_ = 0;
    tracker_.Reset();
    wait_tracker_.Reset();
  }
  mode_ = mode;
  mode_reason_ = reason;
  SE_LOG_INFO("pacing: {} ({}); display mode {:.3f} Hz, latency {}",
              PacingModeName(mode_), mode_reason_, display_hz_,
              latency_.Latency());
}

FrameClock FramePacer::BeginFrame(bool modal, FixedStepper& stepper) {
  AssertMainThread();
  tick_dt_sec_ = 1.0 / static_cast<double>(std::max(1U, stepper.TickRate()));
  std::uint64_t now = clock_->NowNs();
  const int prev_interval = frame_.interval;
  frame_ = PacerFrame{};
  frame_.frame = ++totals_.frames;
  presented_this_frame_ = false;

  // 1. The fence of frame N - L.
  if (port_ != nullptr && submitted_ > 0) {
    const int back = latency_.Latency();
    if (port_->WaitFrameFence(back, 0) == PresentWait::kTimeout) {
      frame_.fence_pending = true;
      ++totals_.fence_pending_frames;
      if (!modal) {
        const std::uint64_t start = now;
        static_cast<void>(
            port_->WaitFrameFence(back, config_.fence_timeout_ns));
        now = clock_->NowNs();
        frame_.fence_wait_ns = now - start;
        ++totals_.fence_waits;
        totals_.fence_wait_ns += frame_.fence_wait_ns;
      }
    }
  }

  // 2. Present statistics, resolved and counted beside the pacer's source
  // (they become its source below only when the latency wait cannot
  // resolve this frame's present; ADR-066).
  stats_live_ = false;
  PresentTracker::Result stats_result;
  if (port_ != nullptr && caps_.present_stats && mode_ != PacingMode::kVrr) {
    PresentStatistics stats;
    if (port_->GetPresentStats(stats)) {
      stats_live_ = true;
      stats_result = tracker_.OnStats(stats, vblank_period_sec_ * kNsPerSec);
      frame_.stats_resolved = stats_result.resolved;
      frame_.stats_missed = stats_result.missed;
      frame_.stats_reset = stats_result.reset;
      totals_.stats_resolved += stats_result.resolved;
      totals_.stats_missed += stats_result.missed;
      if (stats_result.reset) ++totals_.stats_resets;
    }
  }

  // 3. The mode: re-evaluated twice a second, at once when Paced loses its
  // sampler.
  VblankReading reading = sampler_ ? sampler_(now) : VblankReading{};
  if (port_ != nullptr && (now >= next_evaluation_ns_ ||
                           (mode_ == PacingMode::kPaced && !reading.valid))) {
    Evaluate(now, /*initial=*/false);
    next_evaluation_ns_ = now + ToNs(kReevaluateSec);
  }
  const SamplerSample sample = Sample(reading);

  // 4. The FrameClock.
  if (prev_now_ns_ != 0 && now > prev_now_ns_) {
    const double delta = ToSec(now - prev_now_ns_);
    if (delta < kMaxMeasuredSec) {
      const double per_vblank = delta / std::max(1, prev_interval);
      measured_vblank_sec_ =
          measured_vblank_sec_ == 0.0
              ? per_vblank
              : measured_vblank_sec_ +
                    kMeasureWeight * (per_vblank - measured_vblank_sec_);
    }
  }
  const double display_period = display_hz_ > 0.0 ? 1.0 / display_hz_ : 0.0;
  const PeriodChoice period =
      ChoosePeriod(sample, display_period, measured_vblank_sec_);
  vblank_period_sec_ = period.period_sec;
  const int interval = PresentInterval();

  FrameClock clock;
  clock.modal = modal;
  clock.gpu_fence_pending_at_start = frame_.fence_pending;
  clock.timer_p99_sec = TimerP99Sec();
  std::uint64_t start = now;
  frame_target_ns_ = 0;
  if (config_.fixed_frame_dt_sec > 0.0) {
    // Synthetic: every frame exactly dt, one period (tests, captures).
    const std::uint64_t dt_ns = ToNs(config_.fixed_frame_dt_sec);
    synthetic_ns_ = synthetic_ns_ == 0 ? now : synthetic_ns_ + dt_ns;
    start = synthetic_ns_;
    clock.period_sec = config_.fixed_frame_dt_sec;
    clock.vblanks_known = true;
    clock.elapsed_vblanks = 1;
  } else if (mode_ == PacingMode::kVrr) {
    const double vrr_period = 1.0 / VrrRateHz(display_hz_);
    start = vrr_deadline_ns_ != 0 ? vrr_deadline_ns_ : now;
    clock.period_sec = vrr_period;
    clock.vblanks_known = true;
    clock.elapsed_vblanks =
        Intervals(start, prev_start_ns_, vrr_period * kNsPerSec);
    frame_.on_grid = true;
  } else if (period.source == PeriodSource::kSampler && sample.valid) {
    const double vblank_ns = period.period_sec * kNsPerSec;
    const double present_ns = vblank_ns * interval;
    if (mode_ == PacingMode::kPaced && target_ns_ != 0) {
      // One presentation interval before its target (ADR-016).
      const std::uint64_t back = ToNs(present_ns * 1e-9);
      start = target_ns_ > back ? target_ns_ - back : 0;
      frame_target_ns_ = target_ns_;
    } else {
      start = GridFloor(now, reading.last_vblank_ns, vblank_ns);
      // The target: one interval after the latest vblank, where a time just
      // after a true vblank but before the sampler's stamp of it counts as
      // after it (ADR-066); else a present taken at the next vblank would
      // count as one late.
      const std::uint64_t latest =
          GridFloor(now + ToNs(EarlySlackNs(vblank_ns) * 1e-9),
                    reading.last_vblank_ns, vblank_ns);
      frame_target_ns_ = latest + ToNs(present_ns * 1e-9);
    }
    clock.period_sec = period.period_sec * interval;
    clock.vblanks_known = true;
    clock.elapsed_vblanks = Intervals(start, prev_start_ns_, present_ns);
    frame_.on_grid = true;
  } else {
    // The wall clock: the stepper snaps the deltas (M5's frame clock).
    clock.period_sec = period.period_sec * interval;
    clock.vblanks_known = false;
  }
  clock.frame_start_ns = start;
  clock.prev_frame_start_ns = prev_start_ns_;
  clock.real_delta = prev_now_ns_ != 0 && now > prev_now_ns_
                         ? ToSec(now - prev_now_ns_)
                         : clock.period_sec;
  prev_start_ns_ = start;
  prev_now_ns_ = now;

  frame_.mode = mode_;
  frame_.interval = interval;
  frame_.latency = latency_.Latency();
  frame_.period_source = period.source;
  frame_.vblank_period_sec = period.period_sec;
  frame_.period_sec = clock.period_sec;
  frame_.frame_start_ns = start;
  frame_.target_ns = frame_target_ns_;

  // 5. Hits and misses, one result per frame (ADR-066): the latency wait on
  // the vblank grid when it can resolve this frame's present (its result
  // arrives at EndFrame; the previous frame's is reported now), else
  // present statistics, else nothing.
  bool missed = carried_.missed > 0;
  if (port_ != nullptr && caps_.frame_latency_waitable &&
      mode_ != PacingMode::kVrr && frame_target_ns_ != 0) {
    frame_.miss_source = MissSource::kLatencyWait;
  } else if (stats_live_) {
    frame_.miss_source = MissSource::kStatistics;
    frame_.resolved = stats_result.resolved;
    frame_.missed = stats_result.missed;
    frame_.late_vblanks = stats_result.late_refreshes;
    totals_.resolved_presents += stats_result.resolved;
    totals_.resolved_by_statistics += stats_result.resolved;
    totals_.missed_presents += stats_result.missed;
    totals_.missed_vblanks += stats_result.late_refreshes;
    missed = missed || stats_result.missed > 0;
  }
  stepper.OnPresentResult(missed, modal);
  return clock;
}

void FramePacer::RegisterPresent(std::uint64_t target_ns) {
  if (port_ == nullptr || mode_ == PacingMode::kVrr) return;
  // Every present the waitable will wait for, so that the waits line up
  // with their presents.
  if (caps_.frame_latency_waitable) {
    wait_tracker_.OnPresentIssued(target_ns, clock_->NowNs());
  }
  if (!caps_.present_stats || target_ns == 0) return;
  PresentStatistics stats;
  if (!port_->GetPresentStats(stats)) return;
  if (stats.last_present_count != last_present_id_) {
    tracker_.OnPresentIssued(stats.last_present_count, target_ns);
    last_present_id_ = stats.last_present_count;
  }
}

void FramePacer::OnFrameSubmitted(bool presented, bool null_acquire,
                                  SDL_GPUFence* fence) {
  AssertMainThread();
  if (port_ == nullptr) return;
  port_->OnFrameSubmitted(fence);
  ++submitted_;
  frame_.presented = presented;
  frame_.null_acquire = null_acquire;
  presented_this_frame_ = presented;
  frame_.submit_ns = clock_->NowNs();
  if (presented) ++totals_.presented;
  if (null_acquire) ++totals_.null_acquires;
  if (presented) RegisterPresent(frame_target_ns_);
}

void FramePacer::Sleep(std::uint64_t now_ns, std::uint64_t wake_ns) {
  frame_.wake_ns = wake_ns;
  if (wake_ns <= now_ns) {
    frame_.woke_ns = now_ns;
    return;
  }
  clock_->SleepNs(wake_ns - now_ns);
  const std::uint64_t woke = clock_->NowNs();
  frame_.woke_ns = woke;
  frame_.sleep_ns = woke - now_ns;
  ++totals_.sleeps;
  lateness_.Add(woke >= wake_ns ? ToSec(woke - wake_ns)
                                : -ToSec(wake_ns - woke));
}

void FramePacer::WaitWaitable(std::uint64_t& now_ns) {
  if (!caps_.frame_latency_waitable) return;
  const std::uint64_t start = clock_->NowNs();
  // A poll first: only a wait that blocked returned when its present was
  // taken from the queue.
  bool blocked = false;
  PresentWait result = port_->WaitFrameLatency(0);
  if (result == PresentWait::kTimeout) {
    blocked = true;
    result = port_->WaitFrameLatency(config_.waitable_timeout_ns);
  }
  now_ns = clock_->NowNs();
  frame_.waitable = result;
  frame_.waitable_wait_ns += now_ns - start;
  ++totals_.waitable_waits;
  if (result == PresentWait::kTimeout) ++totals_.waitable_timeouts;
  if (mode_ == PacingMode::kVrr) return;
  const WaitTracker::Result waited =
      wait_tracker_.OnWait(result == PresentWait::kReady, blocked, now_ns,
                           latency_.Latency(), vblank_period_sec_ * kNsPerSec);
  if (frame_.miss_source == MissSource::kLatencyWait) AddWaitResult(waited);
}

void FramePacer::AddWaitResult(const WaitTracker::Result& result) {
  frame_.resolved += result.resolved;
  frame_.missed += result.missed;
  frame_.late_vblanks += result.late_refreshes;
  frame_.unresolved += result.unresolved;
  if (result.blocked) {
    frame_.wait_blocked = true;
    frame_.wait_phase_ns = result.phase_ns;
  }
  totals_.resolved_presents += result.resolved;
  totals_.resolved_by_wait += result.resolved;
  totals_.missed_presents += result.missed;
  totals_.missed_vblanks += result.late_refreshes;
  totals_.unresolved_waits += result.unresolved;
  carried_.resolved += result.resolved;
  carried_.missed += result.missed;
  carried_.late_refreshes += result.late_refreshes;
  carried_.unresolved += result.unresolved;
}

void FramePacer::UpdatePolicies(const StepPlan& next, const FrameCosts& costs,
                                bool modal,
                                const WaitTracker::Result& carried) {
  if (mode_ == PacingMode::kVrr || modal) return;
  const double vblank = vblank_period_sec_;
  const double frame_sec = std::clamp(frame_.period_sec, 0.0, kMaxMeasuredSec);
  const double m = SafetyMarginSec(TimerP99Sec(), costs.margin);
  const double ticks_per_vblank = vblank / tick_dt_sec_;
  const double g_hi = std::isnan(costs.gpu_hi) ? 0.0 : costs.gpu_hi;
  if (config_.half_rate) {
    HalfRatePolicy::FrameInput input;
    input.vblank_period_sec = vblank;
    input.frame_sec = frame_sec;
    // Statistics resolved at this frame's start; the latency waits at the
    // previous frame's end.
    input.resolved = frame_.resolved + carried.resolved;
    input.missed = frame_.missed + carried.missed;
    input.planned_full_rate_sec =
        ticks_per_vblank * costs.tick_hi + costs.non_tick_hi + g_hi + m;
    input.full_rate_cost95_sec =
        HalfRateController::FullRateCost95(costs, vblank, tick_dt_sec_, m);
    input.modal = modal;
    if (half_rate_.OnFrame(input)) {
      if (half_rate_.Active()) {
        ++totals_.half_rate_entries;
        SE_LOG_INFO(
            "pacing: HalfRate on at {:.2f} Hz (misses {:.1f}%, "
            "full-rate cost {:.2f} ms)",
            1.0 / vblank, 100.0 * half_rate_.MissFraction(),
            input.planned_full_rate_sec * 1e3);
      } else {
        ++totals_.half_rate_exits;
        SE_LOG_INFO("pacing: HalfRate off ({:.2f} Hz)", 1.0 / vblank);
      }
    }
  }
  const int k = next.k_rt > 0 ? next.k_rt : 1;
  const double cpu = static_cast<double>(k) * costs.tick_95 + costs.non_tick_95;
  const double period = vblank * PresentInterval();
  if (latency_.OnFrame(frame_sec, cpu, costs.gpu_95, period, modal)) {
    ++totals_.latency_changes;
    if (port_ != nullptr && caps_.frame_latency_waitable &&
        !port_->SetFrameLatency(latency_.Latency())) {
      SE_LOG_WARN("pacing: frame latency {} could not be set",
                  latency_.Latency());
    }
    SE_LOG_INFO(
        "pacing: frame latency {} (CPU {:.2f} ms, GPU {:.2f} ms, "
        "period {:.2f} ms)",
        latency_.Latency(), cpu * 1e3, costs.gpu_95 * 1e3, period * 1e3);
  }
}

void FramePacer::EndFrame(const StepPlan& next, const FrameCosts& costs,
                          bool modal) {
  AssertMainThread();
  const WaitTracker::Result carried = std::exchange(carried_, {});
  UpdatePolicies(next, costs, modal, carried);
  frame_.interval = PresentInterval();
  frame_.latency = latency_.Latency();
  std::uint64_t now = clock_->NowNs();
  if (modal || port_ == nullptr || !presented_this_frame_) {
    LogSecond(now);
    return;
  }
  const auto wait_swapchain = [&] {
    const std::uint64_t start = clock_->NowNs();
    static_cast<void>(port_->WaitSwapchain());
    now = clock_->NowNs();
    frame_.swapchain_wait_ns += now - start;
    ++totals_.swapchain_waits;
  };

  switch (mode_) {
    case PacingMode::kPaced: {
      WaitWaitable(now);
      const VblankReading reading = sampler_ ? sampler_(now) : VblankReading{};
      if (!reading.valid) {
        // The sampler went away mid-frame: wait like Standard this once;
        // the next BeginFrame re-evaluates the mode.
        wait_swapchain();
        target_ns_ = 0;
        break;
      }
      const double vblank_ns = vblank_period_sec_ * kNsPerSec;
      const PacedWake wake = PlanPacedWake(
          now, reading.last_vblank_ns, vblank_ns, PresentInterval(), target_ns_,
          next.budget_b, EarlySlackNs(vblank_ns));
      target_ns_ = wake.target_ns;
      Sleep(now, wake.wake_ns);
      break;
    }
    case PacingMode::kStandard: {
      WaitWaitable(now);
      wait_swapchain();
      if (PresentInterval() == 2) {
        // HalfRate: the same composite again, so each frame shows for two
        // vblanks.
        frame_.represented = port_->RepresentLast();
        if (frame_.represented) {
          ++totals_.represents;
          RegisterPresent(frame_target_ns_ != 0
                              ? frame_target_ns_ + ToNs(vblank_period_sec_)
                              : 0);
          WaitWaitable(now);
          wait_swapchain();
        }
      }
      break;
    }
    case PacingMode::kVrr: {
      WaitWaitable(now);
      const std::uint64_t period_ns = ToNs(1.0 / VrrRateHz(display_hz_));
      const double lead_sec =
          std::min(kMaxVrrLeadSec, lateness_.Count() >= kMinLatenessSamples
                                       ? lateness_.Percentile(0.5, 0.0)
                                       : 0.0);
      const VrrDeadline deadline =
          PlanVrrDeadline(now, vrr_deadline_ns_, period_ns, ToNs(lead_sec));
      if (deadline.reanchored) ++totals_.vrr_reanchors;
      Sleep(now, deadline.wake_ns);
      vrr_deadline_ns_ = deadline.deadline_ns;
      frame_.deadline_ns = deadline.deadline_ns;
      frame_.deadline_error_ns =
          frame_.woke_ns >= deadline.deadline_ns
              ? static_cast<std::int64_t>(frame_.woke_ns - deadline.deadline_ns)
              : -static_cast<std::int64_t>(deadline.deadline_ns -
                                           frame_.woke_ns);
      break;
    }
  }
  LogSecond(clock_->NowNs());
}

std::string FramePacer::Summary() const {
  return std::format("{} {:.2f} Hz x{} L{} miss {}", PacingModeName(mode_),
                     vblank_period_sec_ > 0.0 ? 1.0 / vblank_period_sec_ : 0.0,
                     PresentInterval(), latency_.Latency(),
                     totals_.missed_presents);
}

void FramePacer::LogSecond(std::uint64_t now_ns) {
  if (last_log_ns_ == 0) last_log_ns_ = now_ns;
  if (now_ns - last_log_ns_ < kLogIntervalNs) return;
  last_log_ns_ = now_ns;
  const PacerTotals& t = totals_;
  const PacerTotals& l = logged_totals_;
  SE_LOG_DEBUG(
      "pacer: {} {:.3f} Hz ({}) x{} L{}: {} frames, {} presented, {} "
      "waitable waits ({} timeouts), {} fence waits ({:.3f} ms), {} missed "
      "vblanks in {} "
      "of {} presents (misses from {}; {} unresolved waits), {} NULL "
      "acquires, {} sleeps, timer p99 {:.3f} ms",
      PacingModeName(mode_), 1.0 / vblank_period_sec_,
      PeriodSourceName(frame_.period_source), PresentInterval(),
      latency_.Latency(), t.frames - l.frames, t.presented - l.presented,
      t.waitable_waits - l.waitable_waits,
      t.waitable_timeouts - l.waitable_timeouts, t.fence_waits - l.fence_waits,
      static_cast<double>(t.fence_wait_ns - l.fence_wait_ns) * 1e-6,
      t.missed_vblanks - l.missed_vblanks,
      t.missed_presents - l.missed_presents,
      t.resolved_presents - l.resolved_presents,
      MissSourceName(frame_.miss_source),
      t.unresolved_waits - l.unresolved_waits,
      t.null_acquires - l.null_acquires, t.sleeps - l.sleeps,
      TimerP99Sec() * 1e3);
  logged_totals_ = totals_;
}

}  // namespace se
