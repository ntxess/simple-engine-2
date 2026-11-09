// Copyright 2026 The simple-engine-2 Authors.
//
// PacerPolicy: the FramePacer's decisions from numbers
// (docs/milestone-specs.md M6b; docs/design/mt-design.md "Input And
// Latency", "Pacing modes"). Pure: no clock, no SDL, no thread. The shell
// (frame_pacer.h) reads the clock, the present port and the vblank sampler
// and passes what it read in; everything here is unit-tested with made-up
// sequences.
//
//   ChooseMode      Paced, Standard or VRR from the request, the caps, the
//                   sampler and the display mode (the 0.5% rule).
//   ChoosePeriod    the vblank period: the sampler's when it agrees with
//                   the display mode, else the mode, else ADR-039's
//                   measured period.
//   PlanPacedWake   the next frame's target vblank and the wake time,
//                   target - B(next), keeping HalfRate's phase.
//   PlanVrrDeadline VRR's self-paced deadline (120 Hz, 60 Hz below a
//                   120 Hz panel).
//   PresentTracker  hits and misses from present statistics: a present
//                   shown after its target refresh is a miss.
//   HalfRatePolicy  HalfRate above 100 Hz with hysteresis.
//   LatencyPolicy   frame latency 1, or 2 when CPU and GPU each fit in the
//                   period but their sum does not.
//   LatenessTracker the paced sleep's lateness, for m's timer p99.

#ifndef SE_RUNTIME_PACER_POLICY_H_
#define SE_RUNTIME_PACER_POLICY_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "se_core/time.h"
#include "se_runtime/present_port.h"

namespace se {

enum class PacingMode : std::uint8_t { kPaced, kStandard, kVrr };
// --pacing paced|standard|vrr|auto.
enum class PacingRequest : std::uint8_t { kAuto, kPaced, kStandard, kVrr };

[[nodiscard]] std::string_view PacingModeName(PacingMode mode) noexcept;
[[nodiscard]] std::string_view PacingRequestName(
    PacingRequest request) noexcept;
[[nodiscard]] std::optional<PacingRequest> ParsePacingRequest(
    std::string_view text) noexcept;

// The sampler's period and the display mode agree within this fraction.
inline constexpr double kPeriodAgreement = 0.005;
// ADR-039: the measured frame period replaces the mode's beyond this.
inline constexpr double kMeasuredPeriodTolerance = 0.05;
// The refresh assumed when nothing else is known.
inline constexpr double kFallbackRefreshHz = 60.0;
// A sampler whose intervals spread (p90 - p10) more than this fraction of
// the period is not showing a fixed-refresh display (VRR, or a disturbed
// sampler): Paced needs a fixed grid.
inline constexpr double kMaxFixedRefreshSpread = 0.10;

// |measured_sec| is within kPeriodAgreement of |reference_sec| (both > 0).
[[nodiscard]] bool PeriodsAgree(double measured_sec,
                                double reference_sec) noexcept;

// What the vblank sampler reports, as numbers.
struct SamplerSample {
  // Running, fresh (not stale), and the period has enough intervals.
  bool valid = false;
  double period_sec = 0.0;
  // p90 - p10 of the recent intervals.
  double spread_sec = 0.0;
};

// A valid sampler whose intervals are steady enough to call the display
// fixed-refresh.
[[nodiscard]] bool SamplerFixedRefresh(const SamplerSample& sampler) noexcept;

struct ModeInputs {
  PacingRequest request = PacingRequest::kAuto;
  // Paced is this platform's runtime default (Windows). Elsewhere auto is
  // Standard: Linux's Paced from present_wait is not claimed until
  // measured.
  bool paced_by_default = false;
  PresentCaps caps;
  SamplerSample sampler;
  // The display mode's period; 0 when unknown.
  double display_period_sec = 0.0;
};

struct ModeChoice {
  PacingMode mode = PacingMode::kStandard;
  // Why, for the log (a static string).
  std::string_view reason;
};

// Paced when requested or the platform default, and the waitable, present
// statistics and a valid fixed-refresh sampler that agrees with the
// display mode all exist; VRR when requested and tearing is supported;
// Standard otherwise.
[[nodiscard]] ModeChoice ChooseMode(const ModeInputs& inputs) noexcept;

enum class PeriodSource : std::uint8_t {
  kSampler,
  kDisplayMode,
  kMeasured,
  kFallback,
};
[[nodiscard]] std::string_view PeriodSourceName(PeriodSource source) noexcept;

struct PeriodChoice {
  double period_sec = 1.0 / kFallbackRefreshHz;
  PeriodSource source = PeriodSource::kFallback;
};

// The vblank period of the window's display: the sampler's when it is
// valid and agrees with the display mode (or the mode is unknown); else the
// mode, unless the measured frame period (0: none yet) differs from it by
// more than kMeasuredPeriodTolerance (ADR-039); else the measured period;
// 60 Hz when nothing is known.
[[nodiscard]] PeriodChoice ChoosePeriod(const SamplerSample& sampler,
                                        double display_period_sec,
                                        double measured_period_sec) noexcept;

// The latest vblank of the grid anchor + k * period_ns (k any integer) at
// or before |now_ns|.
[[nodiscard]] std::uint64_t GridFloor(std::uint64_t now_ns,
                                      std::uint64_t anchor_ns,
                                      double period_ns) noexcept;
// The grid vblank nearest to |time_ns|.
[[nodiscard]] std::uint64_t GridNearest(std::uint64_t time_ns,
                                        std::uint64_t anchor_ns,
                                        double period_ns) noexcept;

struct PacedWake {
  // The vblank the next frame's present targets.
  std::uint64_t target_ns = 0;
  // When the next frame starts: target - B, never before now.
  std::uint64_t wake_ns = 0;
  // Vblank intervals the target moved past the previous target's phase
  // because they were already past (a late frame).
  std::uint32_t skipped_intervals = 0;
};

// The next frame's target and wake time. |now_ns| is after the waitable;
// |anchor_ns| and |period_ns| are the vblank grid of the window's output;
// |interval| is 1, or 2 in HalfRate; |prev_target_ns| the previous
// frame's target (0: none). The target is the first grid vblank
// |interval| periods after the previous target (HalfRate keeps its phase)
// that is still ahead of now; without a previous target, |interval|
// periods after the vblank at or before now. The wake is target -
// |budget_sec|, or now when that has passed (no sleep).
[[nodiscard]] PacedWake PlanPacedWake(std::uint64_t now_ns,
                                      std::uint64_t anchor_ns, double period_ns,
                                      int interval,
                                      std::uint64_t prev_target_ns,
                                      double budget_sec) noexcept;

// B for a frame of |steps| ticks: steps * S_hi + R_hi + g_hi + m, with an
// unknown g_hi (NaN) counted as 0 and m = SafetyMarginSec(timer p99,
// costs.margin). FixedStepper::PlanNext's budget_b is this, capped at the
// period, plus its miss offset; the pacer sleeps by that one.
[[nodiscard]] double PacedBudgetSec(int steps, const FrameCosts& costs,
                                    double timer_p99_sec) noexcept;

// VRR's deadline rate: 120 Hz, or 60 Hz when the display's mode is below
// 120 Hz (the panel's VRR range does not reach 120 Hz). Unknown (0): 120.
[[nodiscard]] double VrrRateHz(double display_hz) noexcept;

struct VrrDeadline {
  std::uint64_t deadline_ns = 0;
  // deadline - lead (the timer's typical lateness), never before now.
  std::uint64_t wake_ns = 0;
  // The previous deadline had already passed: the next one starts now
  // (no burst of catch-up frames).
  bool reanchored = false;
};

// The next VRR frame start: |period_ns| after the previous deadline (0:
// none, then one period from now).
[[nodiscard]] VrrDeadline PlanVrrDeadline(std::uint64_t now_ns,
                                          std::uint64_t prev_deadline_ns,
                                          std::uint64_t period_ns,
                                          std::uint64_t lead_ns) noexcept;

// Hits and misses from present statistics (DXGI_FRAME_STATISTICS
// semantics). Each issued present is registered with its id (the
// statistics' last_present_count right after it) and its target vblank.
// The statistics name the last displayed present and its refresh; that
// present is a miss when the refresh is later than its target's. A present
// the statistics skipped over (displayed between two samples) is a miss
// only when it must have been late: its target refresh is before the
// earliest refresh it could have been shown on, one after the previous
// displayed present's. Counters that go backwards or a present id that
// restarts (a mode change, a recreated swapchain) reset the tracking.
// Misses are counted against present ids and refresh counts, never frame
// starts.
class PresentTracker {
 public:
  static constexpr std::size_t kCapacity = 16;

  struct Result {
    // Presents the statistics resolved, and the late ones among them.
    std::uint32_t resolved = 0;
    std::uint32_t missed = 0;
    // Refreshes the late ones were shown after their targets, summed.
    std::uint64_t late_refreshes = 0;
    // The statistics restarted: pending presents were dropped.
    bool reset = false;
  };

  // A present was issued with id |present_id| for the vblank |target_ns|.
  // A full queue drops its oldest entry (counted in Dropped()).
  void OnPresentIssued(std::uint64_t present_id,
                       std::uint64_t target_ns) noexcept;
  // Resolves the pending presents |stats| shows displayed, with the
  // display's vblank period |period_ns|.
  Result OnStats(const PresentStatistics& stats, double period_ns) noexcept;
  void Reset() noexcept;

  [[nodiscard]] std::size_t Pending() const noexcept { return size_; }
  [[nodiscard]] std::uint64_t Dropped() const noexcept { return dropped_; }
  [[nodiscard]] std::uint64_t Resets() const noexcept { return resets_; }

 private:
  struct Entry {
    std::uint64_t id = 0;
    std::uint64_t target_ns = 0;
  };
  void PopFront() noexcept;

  std::array<Entry, kCapacity> entries_{};
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  bool have_last_ = false;
  PresentStatistics last_;
  std::uint64_t dropped_ = 0;
  std::uint64_t resets_ = 0;
};

// HalfRate above 100 Hz, with hysteresis (docs/milestone-specs.md M6b):
// entered when, over the last 2 s, more than 1% of the resolved presents
// missed, or when a full-rate frame's planned cost (k_rt * S_hi + R_hi +
// g_hi + m) has exceeded the vblank period for 2 s; left after 4 s of
// margin, in which the full-rate p95 cost (k_rt * S_95 + R_95 + g_95 + m)
// stayed below 0.75 of the vblank period. Modal frames neither count nor
// reset the timers. At or below 100 Hz it is never active.
class HalfRatePolicy {
 public:
  static constexpr double kMinHz = 100.0;
  static constexpr double kEnterSec = 2.0;
  static constexpr double kExitSec = 4.0;
  static constexpr double kMissBudget = 0.01;
  static constexpr double kExitLoadFraction = 0.75;

  struct FrameInput {
    // The display's own vblank period (not doubled).
    double vblank_period_sec = 1.0 / 60.0;
    // This frame's length.
    double frame_sec = 1.0 / 60.0;
    // Presents resolved by this frame's statistics, and how many missed.
    std::uint32_t resolved = 0;
    std::uint32_t missed = 0;
    double planned_full_rate_sec = 0.0;
    double full_rate_cost95_sec = 0.0;
    bool modal = false;
  };

  // Returns true when Active() changed.
  bool OnFrame(const FrameInput& frame) noexcept;
  void Reset() noexcept;

  [[nodiscard]] bool Active() const noexcept { return active_; }
  // Vblanks per present: 1, or 2 in HalfRate.
  [[nodiscard]] int PresentInterval() const noexcept { return active_ ? 2 : 1; }
  [[nodiscard]] std::uint32_t Entries() const noexcept { return entries_; }
  [[nodiscard]] std::uint32_t Exits() const noexcept { return exits_; }
  // The missed fraction over the last window (0 without presents).
  [[nodiscard]] double MissFraction() const noexcept;

 private:
  static constexpr std::size_t kWindowFrames = 2048;
  struct Sample {
    double frame_sec = 0.0;
    std::uint32_t resolved = 0;
    std::uint32_t missed = 0;
  };
  void ClearWindow() noexcept;

  std::array<Sample, kWindowFrames> window_{};
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  double window_sec_ = 0.0;
  std::uint64_t window_resolved_ = 0;
  std::uint64_t window_missed_ = 0;
  double over_budget_sec_ = 0.0;
  double margin_sec_ = 0.0;
  bool active_ = false;
  std::uint32_t entries_ = 0;
  std::uint32_t exits_ = 0;
};

// Frame latency: |base| (1, or 2 for the editor), or 2 when the CPU and
// GPU costs each fit in the presentation period but their sum does not,
// held for 2 s; back to |base| after 4 s without that. An unknown GPU cost
// (NaN) never asks for 2. Not adaptive: always |base|.
class LatencyPolicy {
 public:
  static constexpr double kEnterSec = 2.0;
  static constexpr double kExitSec = 4.0;

  explicit LatencyPolicy(int base = 1, bool adaptive = true) noexcept;

  // Returns true when Latency() changed.
  bool OnFrame(double frame_sec, double cpu_sec, double gpu_sec,
               double period_sec, bool modal) noexcept;
  void Reset() noexcept;

  [[nodiscard]] int Latency() const noexcept { return latency_; }
  [[nodiscard]] int Base() const noexcept { return base_; }
  [[nodiscard]] std::uint32_t Changes() const noexcept { return changes_; }

 private:
  int base_ = 1;
  bool adaptive_ = true;
  int latency_ = 1;
  double want_sec_ = 0.0;
  double clear_sec_ = 0.0;
  std::uint32_t changes_ = 0;
};

// The paced sleep's lateness (wake time minus requested wake time): its
// p99 is the timer p99 in m (docs/design/mt-design.md: m = max(1.5 ms,
// timer p99 + 0.5 ms)), its median the lead VRR aims early by.
class LatenessTracker {
 public:
  static constexpr std::size_t kCapacity = 256;

  // A negative lateness (an early wake) counts as 0.
  void Add(double late_sec) noexcept;
  [[nodiscard]] std::size_t Count() const noexcept { return size_; }
  // The nearest-rank percentile p in [0, 1]; |empty| without samples.
  [[nodiscard]] double Percentile(double p, double empty) const noexcept;

 private:
  std::array<double, kCapacity> samples_{};
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

}  // namespace se

#endif  // SE_RUNTIME_PACER_POLICY_H_
