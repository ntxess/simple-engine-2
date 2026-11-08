// Copyright 2026 The simple-engine-2 Authors.
//
// The formulas are documented in se_core/time.h and come from
// docs/design/mt-design.md ("Catch-up, cadence and overload"), with ADR-008.

#include "se_core/time.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "se_core/assert.h"

namespace se {
namespace {

constexpr double kLockTolerance = 0.005;
constexpr double kStallSec = 0.25;
constexpr double kDebuggerStallSec = 0.1;
constexpr double kRepaySec = 0.25;
constexpr double kMinMarginSec = 1.5e-3;
constexpr double kTimerMarginSec = 0.5e-3;
constexpr double kMinSnapSec = 1.0e-3;
constexpr double kSnapTimerSec = 0.1e-3;
constexpr double kMissOffsetStepSec = 1.0e-3;
constexpr double kMissOffsetDecaySec = 0.05e-3;
constexpr double kOverloadEnterSec = 1.0;
constexpr double kOverloadExitSec = 1.0;
constexpr int kOverloadMisses = 2;
constexpr double kNsPerSec = 1e9;

// Rounds a non-negative value to the nearest integer (halves away from 0).
std::uint64_t RoundNonNegative(double x) {
  return x <= 0.0 ? 0 : static_cast<std::uint64_t>(std::llround(x));
}

// floor() and ceil() for the small step counts used here, clamped so the
// conversions cannot overflow.
int FloorToInt(double x) {
  const double clamped = std::clamp(x, -1024.0, 1024.0);
  const auto i = static_cast<int>(clamped);
  return clamped < static_cast<double>(i) ? i - 1 : i;
}

int CeilToInt(double x) {
  const double clamped = std::clamp(x, -1024.0, 1024.0);
  const auto i = static_cast<int>(clamped);
  return clamped > static_cast<double>(i) ? i + 1 : i;
}

struct CadenceInfo {
  Cadence cadence = Cadence::kFree;
  // Ticks per presentation interval (Locked, T/dt case).
  int k = 0;
  // Presentation intervals per tick (Locked, dt/T case); 1 otherwise.
  std::uint32_t div = 1;
};

CadenceInfo ClassifyCadence(double t, double dt) {
  const double ratio = t / dt;
  const auto k = static_cast<double>(RoundNonNegative(ratio));
  if (k >= 1.0 && std::abs(ratio - k) <= kLockTolerance * ratio) {
    return {Cadence::kLocked, static_cast<int>(k), 1};
  }
  const double inverse = dt / t;
  const auto j = static_cast<double>(RoundNonNegative(inverse));
  if (j >= 2.0 && std::abs(inverse - j) <= kLockTolerance * inverse) {
    return {Cadence::kLocked, 1, static_cast<std::uint32_t>(j)};
  }
  return {};
}

// floor((T - R - g - m) / S); with no tick cost measured yet, the cap.
int KFromCosts(double t, double r, double g, double m, double s) {
  if (!(s > 0.0)) return 2 * kMaxStepsPerFrame;
  return FloorToInt((t - r - g - m) / s);
}

}  // namespace

double SafetyMarginSec(double timer_p99_sec, double extra_margin_sec) noexcept {
  return std::max(kMinMarginSec, timer_p99_sec + kTimerMarginSec) +
         std::max(0.0, extra_margin_sec);
}

FixedStepper::FixedStepper(std::uint32_t tick_rate) noexcept
    : rate_(tick_rate == 0 ? 120 : tick_rate), dt_sec_(1.0 / rate_) {}

StepPlan FixedStepper::Plan(const FrameClock& clock, const FrameCosts& costs,
                            bool running, bool debugger,
                            bool editor_play) noexcept {
  last_running_ = running;
  last_debugger_ = debugger;
  last_editor_play_ = editor_play;

  StepPlan plan;
  const double t = clock.period_sec > 0.0 ? clock.period_sec : dt_sec_;
  const double ratio = t / dt_sec_;
  const CadenceInfo cadence = ClassifyCadence(t, dt_sec_);
  const bool locked = cadence.cadence == Cadence::kLocked;

  // A cadence change (HalfRate, a display change) carries the owed time
  // across: Locked keeps whole ticks of debt, Free an accumulator.
  if (have_cadence_ && cadence.cadence != last_cadence_) {
    if (locked) {
      debt_ += static_cast<std::uint32_t>(acc_units_ / kUnitsPerTick);
      acc_units_ = 0;
      interval_phase_ = 0;
    } else {
      acc_units_ += static_cast<std::uint64_t>(debt_) * kUnitsPerTick;
      debt_ = 0;
    }
    snap_carry_ns_ = 0.0;
  }
  have_cadence_ = true;
  last_cadence_ = cadence.cadence;

  plan.cadence = cadence.cadence;
  plan.locked_k = locked ? cadence.k : 0;
  // ADR-008: in Locked cadence k_rt is the locked ratio, not ceil(T/dt).
  plan.k_rt = locked ? cadence.k : std::max(1, CeilToInt(ratio));

  const double m = SafetyMarginSec(clock.timer_p99_sec, costs.margin);
  const bool gpu_known = !std::isnan(costs.gpu_hi);
  const double g_hi = gpu_known ? costs.gpu_hi : 0.0;
  const double g_95 = std::isnan(costs.gpu_95) ? 0.0 : costs.gpu_95;
  const int k_hi = KFromCosts(t, costs.non_tick_hi, g_hi, m, costs.tick_hi);
  const int k_95 = KFromCosts(t, costs.non_tick_95, g_95, m, costs.tick_95);

  // The frame delta; a stall counts as paused time and as one period.
  const std::uint64_t period_ns = RoundNonNegative(t * kNsPerSec);
  std::uint64_t delta_ns = period_ns;
  if (clock.prev_frame_start_ns != 0) {
    delta_ns = clock.frame_start_ns > clock.prev_frame_start_ns
                   ? clock.frame_start_ns - clock.prev_frame_start_ns
                   : 0;
  }
  const double stall_sec = debugger ? kDebuggerStallSec : kStallSec;
  const bool stalled = static_cast<double>(delta_ns) > stall_sec * kNsPerSec;
  if (stalled) {
    stats_.paused_sec += static_cast<double>(delta_ns - period_ns) / kNsPerSec;
    ++stats_.stalls;
    delta_ns = period_ns;
  }
  const double frame_sec = static_cast<double>(delta_ns) / kNsPerSec;

  // The Overloaded timers count running time only: a pause neither enters
  // nor leaves Overloaded, and both conditions start over after it.
  if (running) {
    UpdateOverload(k_95 < plan.k_rt, frame_sec, clock.modal);
  } else {
    below_rt_sec_ = 0.0;
    clean_sec_ = 0.0;
  }
  // No frame runs more than kMaxStepsPerFrame ticks (StepPlan has that many
  // windows). Below 15 Hz k_rt exceeds it, and debt or dilation takes the
  // rest.
  const int k_rt_capped = std::min(plan.k_rt, kMaxStepsPerFrame);
  if (overloaded_) {
    plan.k_max = std::clamp(k_95, editor_play ? 0 : 1, k_rt_capped);
  } else {
    plan.k_max = std::clamp(std::max(plan.k_rt, k_hi), 1,
                            std::min(kMaxStepsPerFrame, plan.k_rt + 1));
    if (!gpu_known && clock.gpu_fence_pending_at_start) {
      plan.k_max = k_rt_capped;
    }
  }
  plan.overloaded = overloaded_;
  ++stats_.frames;

  std::uint64_t steps = 0;
  std::uint64_t debt = 0;
  std::uint64_t fraction_units = 0;
  if (!running) {
    // Paused: no ticks, no accumulated time, no debt.
    debt_ = 0;
    acc_units_ %= kUnitsPerTick;
    stats_.paused_sec += frame_sec;
  } else {
    std::uint64_t due = 0;
    if (locked) {
      std::uint64_t intervals = 1;
      if (!stalled) {
        if (clock.vblanks_known) {
          intervals = clock.elapsed_vblanks;
        } else if (clock.prev_frame_start_ns != 0) {
          intervals = RoundNonNegative(static_cast<double>(delta_ns) /
                                       static_cast<double>(period_ns));
        }
      }
      std::uint64_t interval_ticks = 0;
      if (cadence.div == 1) {
        interval_ticks = static_cast<std::uint64_t>(cadence.k) * intervals;
      } else {
        const std::uint64_t phase = interval_phase_ + intervals;
        interval_ticks = phase / cadence.div;
        interval_phase_ = static_cast<std::uint32_t>(phase % cadence.div);
      }
      if (!stalled) {
        stats_.residue_sec += static_cast<double>(intervals) * t -
                              static_cast<double>(interval_ticks) * dt_sec_;
      }
      due = debt_ + interval_ticks;
    } else {
      // Snap the delta to a whole number of periods when it is within the
      // timer's error of one; the sub-nanosecond carry keeps exactness.
      const double window_ns =
          std::max(kMinSnapSec, clock.timer_p99_sec + kSnapTimerSec) *
          kNsPerSec;
      const double exact_period_ns = t * kNsPerSec;
      const auto periods = static_cast<double>(
          RoundNonNegative(static_cast<double>(delta_ns) / exact_period_ns));
      double delta_exact = static_cast<double>(delta_ns);
      if (periods >= 1.0 &&
          std::abs(delta_exact - periods * exact_period_ns) <= window_ns) {
        delta_exact = periods * exact_period_ns + snap_carry_ns_;
        const auto whole = static_cast<std::uint64_t>(delta_exact);
        snap_carry_ns_ = delta_exact - static_cast<double>(whole);
        acc_units_ += whole * rate_;
      } else {
        snap_carry_ns_ = 0.0;
        acc_units_ += delta_ns * rate_;
      }
      due = acc_units_ / kUnitsPerTick;
      fraction_units = acc_units_ % kUnitsPerTick;
    }

    SE_ASSERT_MSG(plan.k_max >= 0 && plan.k_max <= kMaxStepsPerFrame,
                  "k_max outside [0, kMaxStepsPerFrame]");
    // Clamped again so that no build can write past tick_window_end_ns.
    steps =
        std::min<std::uint64_t>(due, static_cast<std::uint64_t>(std::clamp(
                                         plan.k_max, 0, kMaxStepsPerFrame)));
    debt = due - steps;

    // Debt the spare rate cannot repay within 0.25 s becomes dilation.
    const double spare = static_cast<double>(plan.k_max) - ratio;
    const std::uint64_t repayable =
        spare > 0.0 ? static_cast<std::uint64_t>(spare * kRepaySec / t) : 0;
    if (debt > repayable) {
      const std::uint64_t excess = debt - repayable;
      stats_.dilated_ticks += excess;
      stats_.debt_dilation_sec += static_cast<double>(excess) * dt_sec_;
      debt = repayable;
    }
    if (locked) {
      debt_ = static_cast<std::uint32_t>(debt);
    } else {
      acc_units_ = debt * kUnitsPerTick + fraction_units;
    }
  }

  plan.steps = static_cast<int>(steps);
  plan.debt_ticks = static_cast<std::uint32_t>(debt);
  last_debt_ticks_ = plan.debt_ticks;
  stats_.ticks += steps;
  if (locked || !running || debt > 0) {
    plan.alpha = 1.0F;
  } else {
    plan.alpha = static_cast<float>(static_cast<double>(fraction_units) /
                                    static_cast<double>(kUnitsPerTick));
  }

  plan.budget_b = std::min(t, static_cast<double>(steps) * costs.tick_hi +
                                  costs.non_tick_hi + g_hi + m) +
                  miss_offset_sec_;
  miss_offset_sec_ = std::max(0.0, miss_offset_sec_ - kMissOffsetDecaySec);

  // The input window of each tick.
  if (steps > 0) {
    const std::uint64_t start = clock.frame_start_ns;
    if (locked) {
      const std::uint64_t span = std::min(delta_ns, start);
      const std::uint64_t prev = start - span;
      for (std::uint64_t i = 0; i < steps; ++i) {
        plan.tick_window_end_ns[i] = prev + span * (i + 1) / steps;
      }
    } else {
      const std::uint64_t remainder_ns = acc_units_ / rate_;
      for (std::uint64_t i = 0; i < steps; ++i) {
        const std::uint64_t back =
            remainder_ns + (steps - 1 - i) * 1'000'000'000ULL / rate_;
        plan.tick_window_end_ns[i] = back < start ? start - back : 0;
      }
    }
  }

  plan.dilation_sec = DilationSec();
  missed_since_plan_ = false;
  last_period_sec_ = t;
  return plan;
}

StepPlan FixedStepper::PlanNext(const FrameClock& clock,
                                const FrameCosts& costs) const noexcept {
  const double t = clock.period_sec > 0.0 ? clock.period_sec : dt_sec_;
  FrameClock next = clock;
  next.prev_frame_start_ns = clock.frame_start_ns;
  next.frame_start_ns = clock.frame_start_ns + RoundNonNegative(t * kNsPerSec);
  next.elapsed_vblanks = 1;
  next.real_delta = t;
  FixedStepper copy = *this;
  return copy.Plan(next, costs, last_running_, last_debugger_,
                   last_editor_play_);
}

void FixedStepper::OnPresentResult(bool missed, bool modal) noexcept {
  if (modal || !missed) {
    consecutive_misses_ = 0;
    return;
  }
  missed_since_plan_ = true;
  ++consecutive_misses_;
  miss_offset_sec_ =
      std::min(miss_offset_sec_ + kMissOffsetStepSec, last_period_sec_);
  if (!overloaded_ && consecutive_misses_ >= kOverloadMisses &&
      last_debt_ticks_ > 0) {
    EnterOverload();
  }
}

void FixedStepper::ResetAccumulator() noexcept {
  acc_units_ = 0;
  debt_ = 0;
  interval_phase_ = 0;
  snap_carry_ns_ = 0.0;
  last_debt_ticks_ = 0;
}

void FixedStepper::UpdateOverload(bool k95_below_rt, double frame_sec,
                                  bool modal) noexcept {
  if (!overloaded_) {
    below_rt_sec_ = k95_below_rt && !modal ? below_rt_sec_ + frame_sec : 0.0;
    if (below_rt_sec_ >= kOverloadEnterSec) EnterOverload();
    return;
  }
  clean_sec_ =
      !k95_below_rt && !missed_since_plan_ ? clean_sec_ + frame_sec : 0.0;
  if (clean_sec_ >= kOverloadExitSec) {
    overloaded_ = false;
    ++stats_.overload_exits;
    clean_sec_ = 0.0;
    below_rt_sec_ = 0.0;
  }
}

void FixedStepper::EnterOverload() noexcept {
  overloaded_ = true;
  ++stats_.overload_entries;
  consecutive_misses_ = 0;
  below_rt_sec_ = 0.0;
  clean_sec_ = 0.0;
}

// CostTracker.

CostTracker::Ring::Ring() : samples_(kCapacity) {}

void CostTracker::Ring::Push(double time, double value) noexcept {
  samples_[head_] = Sample{time, value};
  head_ = (head_ + 1) % kCapacity;
  size_ = std::min(size_ + 1, kCapacity);
}

void CostTracker::Ring::Clear() noexcept {
  head_ = 0;
  size_ = 0;
}

double CostTracker::Ring::Percentile(
    double p, double now, double window_sec, double empty,
    std::vector<double>& scratch) const noexcept {
  std::size_t n = 0;
  for (std::size_t i = 0; i < size_; ++i) {
    const Sample& s = samples_[(head_ + kCapacity - 1 - i) % kCapacity];
    if (s.time <= now - window_sec) break;
    scratch[n++] = s.value;
  }
  if (n == 0) return empty;
  // Nearest rank: the ceil(p * n)-th smallest value.
  auto rank = static_cast<std::size_t>(CeilToInt(p * static_cast<double>(n)));
  rank = std::clamp<std::size_t>(rank, 1, n);
  const auto first = scratch.begin();
  const auto nth = first + static_cast<std::ptrdiff_t>(rank - 1);
  std::nth_element(first, nth, first + static_cast<std::ptrdiff_t>(n));
  return *nth;
}

CostTracker::CostTracker() : scratch_(kCapacity) {}

void CostTracker::AddTick(double tick_sec) noexcept {
  ticks_.Push(now_, tick_sec);
}

void CostTracker::AddFrame(double frame_delta_sec, double non_tick_sec,
                           double gpu_sec) noexcept {
  now_ += std::max(0.0, frame_delta_sec);
  non_ticks_.Push(now_, non_tick_sec);
  if (!std::isnan(gpu_sec)) gpus_.Push(now_, gpu_sec);
}

FrameCosts CostTracker::Current() const noexcept {
  constexpr double kUnknown = std::numeric_limits<double>::quiet_NaN();
  FrameCosts costs;
  costs.tick_hi =
      ticks_.Percentile(kHiPercentile, now_, kHiWindowSec, 0.0, scratch_);
  costs.non_tick_hi =
      non_ticks_.Percentile(kHiPercentile, now_, kHiWindowSec, 0.0, scratch_);
  costs.gpu_hi =
      gpus_.Percentile(kHiPercentile, now_, kHiWindowSec, kUnknown, scratch_);
  costs.tick_95 =
      ticks_.Percentile(kLowPercentile, now_, kLowWindowSec, 0.0, scratch_);
  costs.non_tick_95 =
      non_ticks_.Percentile(kLowPercentile, now_, kLowWindowSec, 0.0, scratch_);
  costs.gpu_95 =
      gpus_.Percentile(kLowPercentile, now_, kLowWindowSec, kUnknown, scratch_);
  return costs;
}

double CostTracker::TickPercentile(double p, double window_sec,
                                   double empty) const noexcept {
  return ticks_.Percentile(p, now_, window_sec, empty, scratch_);
}

void CostTracker::Reset() noexcept {
  ticks_.Clear();
  non_ticks_.Clear();
  gpus_.Clear();
  now_ = 0.0;
}

// HalfRateController.

double HalfRateController::FullRateCost95(const FrameCosts& costs,
                                          double vblank_period_sec,
                                          double tick_dt_sec,
                                          double margin_sec) noexcept {
  const double ticks_per_frame = vblank_period_sec / tick_dt_sec;
  const double gpu = std::isnan(costs.gpu_95) ? 0.0 : costs.gpu_95;
  return ticks_per_frame * costs.tick_95 + costs.non_tick_95 + gpu + margin_sec;
}

void HalfRateController::ClearWindow() noexcept {
  head_ = 0;
  size_ = 0;
}

void HalfRateController::OnFrame(double vblank_period_sec,
                                 double frame_delta_sec, bool missed,
                                 bool modal,
                                 double full_rate_cost95_sec) noexcept {
  const bool high_rate =
      vblank_period_sec > 0.0 && vblank_period_sec < 1.0 / kMinHz;
  if (!high_rate) {
    if (active_) {
      active_ = false;
      ++exits_;
    }
    headroom_sec_ = 0.0;
    ClearWindow();
    return;
  }
  if (modal) return;  // misses do not count while Modal

  if (!active_) {
    window_[head_] = FrameEntry{frame_delta_sec, missed};
    head_ = (head_ + 1) % kWindowFrames;
    size_ = std::min(size_ + 1, kWindowFrames);
    double time = 0.0;
    std::size_t frames = 0;
    std::size_t misses = 0;
    for (std::size_t i = 0; i < size_ && time < kEnterWindowSec; ++i) {
      const FrameEntry& e =
          window_[(head_ + kWindowFrames - 1 - i) % kWindowFrames];
      time += e.delta;
      ++frames;
      if (e.missed) ++misses;
    }
    if (time >= kEnterWindowSec &&
        static_cast<double>(misses) >
            kEnterMissFraction * static_cast<double>(frames)) {
      active_ = true;
      ++entries_;
      headroom_sec_ = 0.0;
      ClearWindow();
    }
    return;
  }

  headroom_sec_ = full_rate_cost95_sec < kExitLoadFraction * vblank_period_sec
                      ? headroom_sec_ + frame_delta_sec
                      : 0.0;
  if (headroom_sec_ >= kExitHeadroomSec) {
    active_ = false;
    ++exits_;
    headroom_sec_ = 0.0;
    ClearWindow();
  }
}

}  // namespace se
