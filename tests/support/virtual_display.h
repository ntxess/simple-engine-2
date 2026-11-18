// Copyright 2026 The simple-engine-2 Authors.
//
// A cost-model frame simulator for the stepper scenarios: a display with
// vblanks at a fixed rate, a loop whose ticks and render cost what the model
// says, and presents that miss when the frame is not ready by its vblank.
//
// The loop, per frame:
//   1. wake at time t; plan the frame (SteppingPolicy::Plan);
//   2. run the planned ticks (cost S each, plus a spike when one is due),
//      then the non-tick CPU work R (plus a preemption when one is drawn),
//      then the GPU work g;
//   3. the frame targets the first presentation vblank after t; it is late
//      (a hitch) when ready after that vblank, and presents at the first
//      vblank after it is ready;
//   4. wake for the next frame: Paced mode at the next target minus the
//      policy's lead (B of the next frame's plan) plus timer lateness, never
//      before the present; Standard mode at the present.
//
// The FrameClock carries frame times on the vblank grid, as the pacer
// reports them from present statistics: a frame's start is one presentation
// interval before its target vblank, and elapsed_vblanks counts the
// intervals between targets. (Wake-time deltas would vary with B from frame
// to frame and defeat the snapping of Free cadence.) real_delta is the
// wall-clock time between loop iterations, which the old accumulator loop
// used.
//
// Tick costs: S = p50 + tail * u^6 with u uniform, so p50 is close to the
// given p50 and p99 is p50 + 0.94 * tail (the rig measured p50 4.20 ms and
// p99 5.03 ms at N=50k). Every draw comes from se::rng with the model's
// seed, so a run is reproducible.

#ifndef TESTS_SUPPORT_VIRTUAL_DISPLAY_H_
#define TESTS_SUPPORT_VIRTUAL_DISPLAY_H_

#include <cstdint>
#include <vector>

#include "se_core/time.h"

namespace se::testing {

struct CostModel {
  double tick_p50_sec = 4.2e-3;
  double tick_tail_sec = 0.85e-3;
  double non_tick_sec = 1.4e-3;
  // Real GPU time after submit, and whether the pacer can measure it.
  double gpu_sec = 0.0;
  bool gpu_known = true;
  // One tick costs spike_sec more every spike_period_sec (0: none).
  double spike_sec = 0.0;
  double spike_period_sec = 0.0;
  // A frame is preempted for preempt_sec with this probability.
  double preempt_probability = 0.0;
  double preempt_sec = 0.0;
  // Paced sleeps wake up to this late (uniform).
  double timer_late_max_sec = 0.6e-3;
  double timer_p99_sec = 0.6e-3;
  std::uint64_t seed = 1;
};

// The N=50k costs (S 4.2 ms, R 1.4 ms).
[[nodiscard]] CostModel CostsN50k();
// The N=30k costs (S 2.59 ms, R 0.84 ms).
[[nodiscard]] CostModel CostsN30k();

struct DisplayConfig {
  double hz = 60.0;
  // Paced: wake at vblank - B(next). Standard: wake at the present.
  bool paced = true;
};

struct FrameRecord {
  std::uint64_t start_ns = 0;
  std::uint64_t present_ns = 0;
  int steps = 0;
  int k_max = 0;
  int k_rt = 0;
  int present_interval = 1;
  bool late = false;
  std::uint32_t missed_vblanks = 0;
  Cadence cadence = Cadence::kFree;
  bool overloaded = false;
  bool spike = false;
};

struct RunResult {
  std::vector<FrameRecord> frames;
  std::uint64_t ticks = 0;
  std::uint32_t late_frames = 0;
  std::uint64_t missed_vblanks = 0;
  int max_steps = 0;
  std::uint32_t spikes = 0;

  [[nodiscard]] std::uint32_t FramesWithSteps(int steps) const;
  [[nodiscard]] std::uint32_t LateFramesAfter(double seconds) const;
  // Late frames within |window_sec| after each spike frame, the spike frame
  // included; the largest count over all spikes.
  [[nodiscard]] std::uint32_t MaxLateFramesPerSpike(double window_sec) const;
};

// The stepping policy under test.
class SteppingPolicy {
 public:
  virtual ~SteppingPolicy() = default;
  virtual StepPlan Plan(const FrameClock& clock, const FrameCosts& costs) = 0;
  virtual void OnPresent(bool late) = 0;
  // Paced mode: how long before the next target vblank to wake.
  virtual double WakeLeadSec(const FrameClock& clock,
                             const FrameCosts& costs) = 0;
};

// The engine's FixedStepper, waking at B of PlanNext().
class FixedStepperPolicy final : public SteppingPolicy {
 public:
  explicit FixedStepperPolicy(bool editor_play = false)
      : editor_play_(editor_play) {}
  StepPlan Plan(const FrameClock& clock, const FrameCosts& costs) override;
  void OnPresent(bool late) override;
  double WakeLeadSec(const FrameClock& clock, const FrameCosts& costs) override;
  [[nodiscard]] const FixedStepper& Stepper() const { return stepper_; }

 private:
  FixedStepper stepper_;
  bool editor_play_;
};

// The loop the design replaces: a wall-clock accumulator in seconds with a
// 0.25 s clamp and at most max_steps steps, waking a fixed lead before the
// vblank (Standard mode: at the present).
class AccumulatorCapPolicy final : public SteppingPolicy {
 public:
  AccumulatorCapPolicy(int max_steps, double fixed_lead_sec)
      : max_steps_(max_steps), fixed_lead_sec_(fixed_lead_sec) {}
  StepPlan Plan(const FrameClock& clock, const FrameCosts& costs) override;
  void OnPresent(bool late) override;
  double WakeLeadSec(const FrameClock& clock, const FrameCosts& costs) override;

 private:
  int max_steps_;
  double fixed_lead_sec_;
  double acc_sec_ = 0.0;
};

// Runs |policy| against the display for |seconds| of wall time. With a
// HalfRateController, presents go to every second vblank while it is active.
[[nodiscard]] RunResult RunVirtualDisplay(
    const DisplayConfig& display, const CostModel& model,
    SteppingPolicy& policy, double seconds,
    HalfRateController* half_rate = nullptr);

}  // namespace se::testing

#endif  // TESTS_SUPPORT_VIRTUAL_DISPLAY_H_
