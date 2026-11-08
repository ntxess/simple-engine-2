// Copyright 2026 The simple-engine-2 Authors.

#include "support/virtual_display.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "se_core/guid.h"
#include "se_core/rng.h"
#include "se_core/time.h"

namespace se::testing {
namespace {

constexpr double kNsPerSec = 1e9;
constexpr double kTickDt = 1.0 / 120.0;

// Draw streams of the cost model.
enum Stream : std::uint32_t {
  kTickStream = 1,
  kPreemptStream = 2,
  kTimerStream = 3,
};

float Draw(const CostModel& model, std::uint64_t frame, std::uint32_t stream,
           std::uint32_t index) {
  return rng::Unit(rng::At(model.seed, frame, Guid{0, index}, stream, 0));
}

double TickCost(const CostModel& model, std::uint64_t frame, int tick) {
  const double u =
      Draw(model, frame, kTickStream, static_cast<std::uint32_t>(tick));
  const double u2 = u * u;
  return model.tick_p50_sec + model.tick_tail_sec * u2 * u2 * u2;
}

}  // namespace

CostModel CostsN50k() { return CostModel{}; }

CostModel CostsN30k() {
  CostModel model;
  model.tick_p50_sec = 2.59e-3;
  model.tick_tail_sec = 0.5e-3;
  model.non_tick_sec = 0.84e-3;
  return model;
}

std::uint32_t RunResult::FramesWithSteps(int steps) const {
  return static_cast<std::uint32_t>(std::count_if(
      frames.begin(), frames.end(),
      [steps](const FrameRecord& f) { return f.steps == steps; }));
}

std::uint32_t RunResult::LateFramesAfter(double seconds) const {
  const auto from = static_cast<std::uint64_t>(seconds * kNsPerSec);
  return static_cast<std::uint32_t>(std::count_if(
      frames.begin(), frames.end(),
      [from](const FrameRecord& f) { return f.late && f.start_ns >= from; }));
}

std::uint32_t RunResult::MaxLateFramesPerSpike(double window_sec) const {
  const auto window = static_cast<std::uint64_t>(window_sec * kNsPerSec);
  std::uint32_t worst = 0;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    if (!frames[i].spike) continue;
    std::uint32_t late = 0;
    for (std::size_t j = i;
         j < frames.size() && frames[j].start_ns < frames[i].start_ns + window;
         ++j) {
      if (frames[j].late) ++late;
    }
    worst = std::max(worst, late);
  }
  return worst;
}

StepPlan FixedStepperPolicy::Plan(const FrameClock& clock,
                                  const FrameCosts& costs) {
  return stepper_.Plan(clock, costs, /*running=*/true, /*debugger=*/false,
                       editor_play_);
}

void FixedStepperPolicy::OnPresent(bool late) {
  stepper_.OnPresentResult(late, /*modal=*/false);
}

double FixedStepperPolicy::WakeLeadSec(const FrameClock& clock,
                                       const FrameCosts& costs) {
  return stepper_.PlanNext(clock, costs).budget_b;
}

StepPlan AccumulatorCapPolicy::Plan(const FrameClock& clock,
                                    const FrameCosts& /*costs*/) {
  // The old loop accumulated wall-clock time between iterations.
  acc_sec_ += std::min(clock.real_delta, 0.25);
  int steps = 0;
  while (steps < max_steps_ && acc_sec_ >= kTickDt) {
    acc_sec_ -= kTickDt;
    ++steps;
  }
  StepPlan plan;
  plan.steps = steps;
  plan.k_max = max_steps_;
  plan.k_rt = max_steps_;
  plan.cadence = Cadence::kFree;
  return plan;
}

void AccumulatorCapPolicy::OnPresent(bool /*late*/) {}

double AccumulatorCapPolicy::WakeLeadSec(const FrameClock& /*clock*/,
                                         const FrameCosts& /*costs*/) {
  return fixed_lead_sec_;
}

RunResult RunVirtualDisplay(const DisplayConfig& display,
                            const CostModel& model, SteppingPolicy& policy,
                            double seconds, HalfRateController* half_rate) {
  RunResult result;
  CostTracker tracker;
  const double vblank_ns = kNsPerSec / display.hz;
  // Time is kept on an integer vblank grid, so a frame that wakes at a
  // vblank never mistakes it for its target. The run starts at vblank 1000
  // (a FrameClock start of 0 would mean "no previous frame").
  constexpr std::uint64_t kFirstVblank = 1000;
  const auto end_vblank =
      kFirstVblank + static_cast<std::uint64_t>(seconds * display.hz);
  auto vblank_time = [vblank_ns](std::uint64_t v) {
    return static_cast<double>(v) * vblank_ns;
  };
  double next_spike_sec =
      vblank_time(kFirstVblank) / kNsPerSec +
      (model.spike_period_sec > 0.0 ? model.spike_period_sec
                                    : std::numeric_limits<double>::infinity());

  std::uint64_t present_vblank = kFirstVblank;  // the last present
  double wake_ns = vblank_time(present_vblank);
  std::uint64_t prev_target = 0;
  std::uint64_t prev_logical_ns = 0;
  double prev_wake_ns = -1.0;
  for (std::uint64_t frame = 0; present_vblank < end_vblank; ++frame) {
    const std::uint64_t interval =
        half_rate != nullptr && half_rate->Active() ? 2 : 1;
    const double interval_ns = vblank_ns * static_cast<double>(interval);

    // The target: the first presentation vblank after the wake.
    std::uint64_t target = present_vblank + interval;
    while (vblank_time(target) <= wake_ns) target += interval;

    // The pacer reports frame times on the vblank grid (present
    // statistics): the frame's logical start is one interval before its
    // target. The wake time only decides when the work runs.
    FrameClock clock;
    clock.frame_start_ns = static_cast<std::uint64_t>(
        std::llround(vblank_time(target) - interval_ns));
    clock.prev_frame_start_ns = prev_logical_ns;
    clock.period_sec = interval_ns / kNsPerSec;
    clock.vblanks_known = true;
    clock.elapsed_vblanks =
        prev_target == 0 ? 1
                         : static_cast<std::uint32_t>(std::max<std::uint64_t>(
                               1, (target - prev_target) / interval));
    clock.timer_p99_sec = model.timer_p99_sec;
    // The wall-clock delta between loop iterations.
    clock.real_delta = prev_wake_ns < 0.0
                           ? clock.period_sec
                           : (wake_ns - prev_wake_ns) / kNsPerSec;

    const FrameCosts costs = tracker.Current();
    const StepPlan plan = policy.Plan(clock, costs);

    FrameRecord record;
    record.start_ns =
        static_cast<std::uint64_t>(wake_ns - vblank_time(kFirstVblank));
    record.steps = plan.steps;
    record.k_max = plan.k_max;
    record.k_rt = plan.k_rt;
    record.cadence = plan.cadence;
    record.overloaded = plan.overloaded;
    record.present_interval = static_cast<int>(interval);

    double cpu_sec = 0.0;
    for (int i = 0; i < plan.steps; ++i) {
      double cost = TickCost(model, frame, i);
      const double now_sec = wake_ns / kNsPerSec + cpu_sec;
      if (now_sec >= next_spike_sec) {
        cost += model.spike_sec;
        next_spike_sec += model.spike_period_sec;
        record.spike = true;
        ++result.spikes;
      }
      tracker.AddTick(cost);
      cpu_sec += cost;
    }
    double non_tick_sec = model.non_tick_sec;
    if (model.preempt_probability > 0.0 &&
        Draw(model, frame, kPreemptStream, 0) < model.preempt_probability) {
      non_tick_sec += model.preempt_sec;
    }
    cpu_sec += non_tick_sec;

    const double ready_ns = wake_ns + (cpu_sec + model.gpu_sec) * kNsPerSec;
    std::uint64_t present = target;
    while (vblank_time(present) < ready_ns) present += interval;
    record.late = present != target;
    record.missed_vblanks = static_cast<std::uint32_t>(present - target);
    record.present_ns = static_cast<std::uint64_t>(vblank_time(present) -
                                                   vblank_time(kFirstVblank));

    policy.OnPresent(record.late);
    tracker.AddFrame(clock.real_delta, non_tick_sec,
                     model.gpu_known
                         ? model.gpu_sec
                         : std::numeric_limits<double>::quiet_NaN());
    if (half_rate != nullptr) {
      const double m = SafetyMarginSec(model.timer_p99_sec, 0.0);
      half_rate->OnFrame(
          vblank_ns / kNsPerSec, clock.real_delta, record.late,
          /*modal=*/false,
          HalfRateController::FullRateCost95(
              tracker.Current(), vblank_ns / kNsPerSec, kTickDt, m));
    }

    result.ticks += static_cast<std::uint64_t>(plan.steps);
    result.max_steps = std::max(result.max_steps, plan.steps);
    if (record.late) ++result.late_frames;
    result.missed_vblanks += record.missed_vblanks;
    result.frames.push_back(record);

    // The next wake: Standard at the present, Paced at the next target
    // minus the planned budget, never before the present; both wake up to
    // the timer's lateness late.
    const std::uint64_t next_interval =
        half_rate != nullptr && half_rate->Active() ? 2 : 1;
    const double present_time = vblank_time(present);
    double next_wake = present_time;
    if (display.paced) {
      const double next_target = vblank_time(present + next_interval);
      const double lead_sec = policy.WakeLeadSec(clock, tracker.Current());
      next_wake = std::max(present_time, next_target - lead_sec * kNsPerSec);
    }
    next_wake += Draw(model, frame, kTimerStream, 0) *
                 model.timer_late_max_sec * kNsPerSec;
    prev_wake_ns = wake_ns;
    prev_target = target;
    prev_logical_ns = clock.frame_start_ns;
    present_vblank = present;
    wake_ns = next_wake;
  }
  return result;
}

}  // namespace se::testing
