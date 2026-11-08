// Copyright 2026 The simple-engine-2 Authors.
//
// Frame stepping: how many fixed ticks each presented frame runs (design
// changes 1 and 2, docs/design/mt-design.md "Catch-up, cadence and
// overload"). Everything here is pure: no clock reads. The pacer passes in
// the frame's timing (FrameClock) and the measured costs (FrameCosts, from
// CostTracker); FixedStepper returns a StepPlan.
//
// Definitions (all times in seconds unless named _ns):
//   dt   the tick period, 1 / tick rate.
//   T    the presentation period (2 vblanks in HalfRate).
//   m    max(1.5 ms, timer p99 + 0.5 ms) + FrameCosts::margin.
//   S, R, g  tick, non-tick CPU and GPU cost; _hi = p99.5 over 4 s,
//            _95 = p95 over 2 s. Unknown g (NaN) counts as 0.
//
// Cadence. Locked when |T/dt - k| <= 0.005 T/dt for an integer k >= 1 (or
// the same on dt/T): k ticks per elapsed presentation interval, alpha = 1;
// the residue against wall time is counted as dilation. Free otherwise: an
// accumulator of frame deltas, alpha = remainder / dt.
//
// Cap. k_rt = k in Locked (ADR-008), ceil(T/dt) in Free.
//   k_hi = floor((T - R_hi - g_hi - m) / S_hi), k_95 likewise with p95.
//   Normal:     k_max = clamp(max(k_rt, k_hi), 1, min(8, k_rt + 1)).
//   Overloaded: k_max = clamp(k_95, editor_play ? 0 : 1, k_rt).
//   Normal with g unknown and the previous fence pending at frame start:
//   k_max = k_rt.
//   steps = min(due, k_max). Locked due = k * elapsed intervals + debt.
//   Free due = accumulator / dt, frame deltas snapped to multiples of T
//   within max(1.0 ms, timer p99 + 0.1 ms).
//
// Accumulator. An integer in ns x tick rate, so one tick is exactly 1e9
// units and 120 ticks are exactly one simulated second.
//
// Debt and dilation. Debt is due minus steps. Debt the spare rate
// (k_max - T/dt ticks per frame) cannot repay within 0.25 s is dropped from
// the accumulator and counted as dilation, keeping the accumulator's
// fraction. No tick is ever skipped: dilation slows the game, it never jumps.
//
// Overloaded. Entered after 2 consecutive missed presents with debt, or
// k_95 < k_rt for 1 s; never while Modal. Left after 1 s with no miss and
// k_95 >= k_rt.
//
// Budget. B = min(T, steps * S_hi + R_hi + g_hi + m) + miss offset, where
// each miss adds 1 ms to the offset and it decays by 0.05 ms per frame. The
// pacer wakes at the predicted vblank minus B of the next frame's plan
// (PlanNext, before the late-latch sleep).
//
// Stalls. A frame delta over 0.25 s (0.1 s with a debugger) is paused time:
// the frame runs as if one period passed, with no catch-up.
//
// Tick windows. Locked splits [prev_start, start) evenly between the steps;
// Free: window_end_i = start - remainder - (steps - 1 - i) * dt.

#ifndef SE_CORE_TIME_H_
#define SE_CORE_TIME_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace se {

enum class Cadence : std::uint8_t { kLocked, kFree };

inline constexpr int kMaxStepsPerFrame = 8;

// The timing of one presented frame, from the pacer.
struct FrameClock {
  std::uint64_t frame_start_ns = 0;
  // 0 on the first frame.
  std::uint64_t prev_frame_start_ns = 0;
  // The presentation period T: one vblank, or two in HalfRate.
  double period_sec = 1.0 / 60.0;
  // Presentation intervals since the previous frame start, when known.
  std::uint32_t elapsed_vblanks = 1;
  bool vblanks_known = false;
  bool modal = false;
  bool gpu_fence_pending_at_start = false;
  double timer_p99_sec = 0.0;
  double real_delta = 0.0;
};

// Measured costs, from CostTracker::Current().
struct FrameCosts {
  double tick_hi = 0.0;
  double non_tick_hi = 0.0;
  double gpu_hi = std::numeric_limits<double>::quiet_NaN();
  double tick_95 = 0.0;
  double non_tick_95 = 0.0;
  double gpu_95 = std::numeric_limits<double>::quiet_NaN();
  // Extra safety margin added to m.
  double margin = 0.0;
};

struct StepPlan {
  int steps = 0;
  int k_max = 0;
  int k_rt = 0;
  // Ticks per presentation interval in Locked cadence; 0 in Free.
  int locked_k = 0;
  Cadence cadence = Cadence::kFree;
  // Interpolation factor for rendering: 1 in Locked cadence.
  float alpha = 1.0F;
  bool overloaded = false;
  // Ticks still owed after this frame's steps.
  std::uint32_t debt_ticks = 0;
  // Total dilation so far (cadence residue plus dropped debt).
  double dilation_sec = 0.0;
  // The CPU budget B for the pacer's wake time.
  double budget_b = 0.0;
  // End of each tick's input window (ns, the FrameClock's clock).
  std::array<std::uint64_t, kMaxStepsPerFrame> tick_window_end_ns{};
};

// Counters for HUDs, logs and tests.
struct StepperStats {
  std::uint64_t frames = 0;
  std::uint64_t ticks = 0;
  // Locked-cadence wall time not covered by ticks (0.1% at 59.94 Hz).
  double residue_sec = 0.0;
  // Debt dropped because it could not be repaid in 0.25 s.
  double debt_dilation_sec = 0.0;
  std::uint64_t dilated_ticks = 0;
  double paused_sec = 0.0;
  std::uint64_t stalls = 0;
  std::uint32_t overload_entries = 0;
  std::uint32_t overload_exits = 0;
};

class FixedStepper {
 public:
  // One tick is this many accumulator units.
  static constexpr std::uint64_t kUnitsPerTick = 1'000'000'000ULL;

  FixedStepper() noexcept = default;
  explicit FixedStepper(std::uint32_t tick_rate) noexcept;

  // Plans the frame. running is false while the simulation is paused (no
  // ticks, no time accumulated); debugger lowers the stall threshold to
  // 0.1 s; editor_play lets Overloaded run 0 ticks.
  StepPlan Plan(const FrameClock& clock, const FrameCosts& costs, bool running,
                bool debugger, bool editor_play) noexcept;

  // Predicts the next frame's plan, assuming it starts one period after
  // |clock| and the costs stay the same. Does not change the stepper.
  [[nodiscard]] StepPlan PlanNext(const FrameClock& clock,
                                  const FrameCosts& costs) const noexcept;

  // Reports whether the frame's present missed its vblank. Misses while
  // Modal do not count.
  void OnPresentResult(bool missed, bool modal) noexcept;

  // Clears the accumulator and the debt (Play start, resume after a load).
  void ResetAccumulator() noexcept;

  [[nodiscard]] bool Overloaded() const noexcept { return overloaded_; }
  [[nodiscard]] const StepperStats& Stats() const noexcept { return stats_; }
  [[nodiscard]] std::uint32_t TickRate() const noexcept { return rate_; }
  [[nodiscard]] std::uint64_t AccumulatorUnits() const noexcept {
    return acc_units_;
  }
  // Total dilation: cadence residue plus dropped debt.
  [[nodiscard]] double DilationSec() const noexcept {
    return stats_.residue_sec + stats_.debt_dilation_sec;
  }

 private:
  // Updates the Overloaded state from this frame's k_95 and delta.
  void UpdateOverload(bool k95_below_rt, double frame_sec, bool modal) noexcept;
  void EnterOverload() noexcept;

  std::uint32_t rate_ = 120;
  double dt_sec_ = 1.0 / 120.0;

  // Free cadence: ns x rate units; Locked cadence: whole ticks in debt_.
  std::uint64_t acc_units_ = 0;
  std::uint32_t debt_ = 0;
  // Locked dt/T cadence (one tick every div intervals): intervals carried.
  std::uint32_t interval_phase_ = 0;
  // Sub-nanosecond part of snapped deltas, so snapping does not drift.
  double snap_carry_ns_ = 0.0;
  bool have_cadence_ = false;
  Cadence last_cadence_ = Cadence::kFree;

  bool overloaded_ = false;
  int consecutive_misses_ = 0;
  bool missed_since_plan_ = false;
  double below_rt_sec_ = 0.0;
  double clean_sec_ = 0.0;
  double miss_offset_sec_ = 0.0;
  std::uint32_t last_debt_ticks_ = 0;
  double last_period_sec_ = 1.0 / 60.0;

  // Flags of the last Plan(), reused by PlanNext().
  bool last_running_ = true;
  bool last_debugger_ = false;
  bool last_editor_play_ = false;

  StepperStats stats_;
};

// Rolling cost percentiles over fixed rings (no allocation after
// construction): p99.5 over the last 4 s and p95 over the last 2 s.
class CostTracker {
 public:
  static constexpr std::size_t kCapacity = 4096;
  static constexpr double kHiWindowSec = 4.0;
  static constexpr double kHiPercentile = 0.995;
  static constexpr double kLowWindowSec = 2.0;
  static constexpr double kLowPercentile = 0.95;

  CostTracker();

  // One tick's cost, at the current time.
  void AddTick(double tick_sec) noexcept;
  // Ends a frame: advances the time by frame_delta_sec and records the
  // non-tick CPU and GPU cost (NaN when the GPU time is unknown).
  void AddFrame(double frame_delta_sec, double non_tick_sec,
                double gpu_sec) noexcept;
  [[nodiscard]] FrameCosts Current() const noexcept;
  void Reset() noexcept;

  // The nearest-rank percentile p of the values recorded in the last
  // window_sec; |empty| when there are none.
  [[nodiscard]] double TickPercentile(double p, double window_sec,
                                      double empty) const noexcept;

 private:
  struct Sample {
    double time = 0.0;
    double value = 0.0;
  };
  class Ring {
   public:
    Ring();
    void Push(double time, double value) noexcept;
    void Clear() noexcept;
    [[nodiscard]] double Percentile(
        double p, double now, double window_sec, double empty,
        std::vector<double>& scratch) const noexcept;

   private:
    std::vector<Sample> samples_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
  };

  Ring ticks_;
  Ring non_ticks_;
  Ring gpus_;
  double now_ = 0.0;
  mutable std::vector<double> scratch_;
};

// HalfRate above 100 Hz, with hysteresis: entered when more than 1% of the
// presents in the last 2 s missed; left after 5 s in which the full-rate
// frame cost p95(C + g) + m stayed below 0.75 of the vblank period.
class HalfRateController {
 public:
  static constexpr double kMinHz = 100.0;
  static constexpr double kEnterMissFraction = 0.01;
  static constexpr double kEnterWindowSec = 2.0;
  static constexpr double kExitHeadroomSec = 5.0;
  static constexpr double kExitLoadFraction = 0.75;

  // One presented frame. vblank_period_sec is the display's own period
  // (not doubled); full_rate_cost95_sec is FullRateCost95().
  void OnFrame(double vblank_period_sec, double frame_delta_sec, bool missed,
               bool modal, double full_rate_cost95_sec) noexcept;

  [[nodiscard]] bool Active() const noexcept { return active_; }
  // Vblanks per present: 1, or 2 in HalfRate.
  [[nodiscard]] int PresentInterval() const noexcept { return active_ ? 2 : 1; }
  [[nodiscard]] std::uint32_t Entries() const noexcept { return entries_; }
  [[nodiscard]] std::uint32_t Exits() const noexcept { return exits_; }

  // p95 of a full-rate frame: (T/dt) ticks of S_95 plus R_95, g_95 and m.
  [[nodiscard]] static double FullRateCost95(const FrameCosts& costs,
                                             double vblank_period_sec,
                                             double tick_dt_sec,
                                             double margin_sec) noexcept;

 private:
  static constexpr std::size_t kWindowFrames = 2048;
  struct FrameEntry {
    double delta = 0.0;
    bool missed = false;
  };

  void ClearWindow() noexcept;

  std::array<FrameEntry, kWindowFrames> window_{};
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  bool active_ = false;
  double headroom_sec_ = 0.0;
  std::uint32_t entries_ = 0;
  std::uint32_t exits_ = 0;
};

// m = max(1.5 ms, timer p99 + 0.5 ms) + extra margin.
[[nodiscard]] double SafetyMarginSec(double timer_p99_sec,
                                     double extra_margin_sec) noexcept;

}  // namespace se

#endif  // SE_CORE_TIME_H_
