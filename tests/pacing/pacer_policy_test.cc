// Copyright 2026 The simple-engine-2 Authors.
//
// PacerPolicy, without a clock or SDL (docs/milestone-specs.md M6b,
// "Tests"): the mode choice per caps, sampler validity and the 0.5% rule,
// the period choice, B and the sleep target from costs, miss counting from
// statistics sequences, HalfRate entry and exit with hysteresis, adaptive
// latency and the VRR deadline.

#include "se_runtime/pacer_policy.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>

#include "se_core/time.h"
#include "se_runtime/present_port.h"

namespace se {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double k165 = 1.0 / 165.003;
constexpr double k60 = 1.0 / 60.0;

PresentCaps PatchedCaps() {
  PresentCaps caps;
  caps.patched = true;
  caps.frame_latency_waitable = true;
  caps.present_stats = true;
  caps.gpu_timestamps = true;
  caps.native_handles = true;
  caps.device_status = true;
  caps.tearing = true;
  return caps;
}

SamplerSample GoodSampler(double period_sec) {
  return SamplerSample{true, period_sec, 0.01 * period_sec};
}

ModeInputs PacedInputs() {
  ModeInputs inputs;
  inputs.paced_by_default = true;
  inputs.caps = PatchedCaps();
  inputs.sampler = GoodSampler(k165);
  inputs.display_period_sec = k165;
  return inputs;
}

TEST_CASE("Pacing requests parse and print", "[pacer]") {
  REQUIRE(ParsePacingRequest("auto") == PacingRequest::kAuto);
  REQUIRE(ParsePacingRequest("paced") == PacingRequest::kPaced);
  REQUIRE(ParsePacingRequest("standard") == PacingRequest::kStandard);
  REQUIRE(ParsePacingRequest("vrr") == PacingRequest::kVrr);
  REQUIRE_FALSE(ParsePacingRequest("Paced").has_value());
  REQUIRE(PacingModeName(PacingMode::kPaced) == "Paced");
  REQUIRE(PacingRequestName(PacingRequest::kVrr) == "vrr");
  REQUIRE(SwapchainModeName(SwapchainMode::kImmediate) == "immediate");
}

TEST_CASE("Paced needs the waitable and an agreeing sampler", "[pacer]") {
  ModeInputs inputs = PacedInputs();
  CHECK(ChooseMode(inputs).mode == PacingMode::kPaced);

  SECTION("the unpatched SDL has no waitable") {
    inputs.caps = PresentCaps{};
    inputs.caps.tearing = true;
    const ModeChoice choice = ChooseMode(inputs);
    CHECK(choice.mode == PacingMode::kStandard);
    CHECK(choice.reason.find("waitable") != std::string_view::npos);
  }
  SECTION("no present statistics: misses from the latency wait") {
    // A window DWM composes on D3D12, or Vulkan without display timing
    // (ADR-066).
    inputs.caps.present_stats = false;
    const ModeChoice choice = ChooseMode(inputs);
    CHECK(choice.mode == PacingMode::kPaced);
    CHECK(choice.reason.find("latency wait") != std::string_view::npos);
  }
  SECTION("no valid sampler") {
    inputs.sampler.valid = false;
    const ModeChoice choice = ChooseMode(inputs);
    CHECK(choice.mode == PacingMode::kStandard);
    CHECK(choice.reason.find("sampler") != std::string_view::npos);
  }
  SECTION("the display mode is unknown") {
    inputs.display_period_sec = 0.0;
    CHECK(ChooseMode(inputs).mode == PacingMode::kStandard);
  }
  SECTION("SDL's 60.001 Hz against a 165 Hz output") {
    inputs.display_period_sec = 1.0 / 60.001;
    const ModeChoice choice = ChooseMode(inputs);
    CHECK(choice.mode == PacingMode::kStandard);
    CHECK(choice.reason.find("0.5%") != std::string_view::npos);
  }
  SECTION("intervals that vary are not a fixed-refresh display") {
    inputs.sampler.spread_sec = 0.2 * k165;
    CHECK(ChooseMode(inputs).mode == PacingMode::kStandard);
  }
  SECTION("auto off Windows is Standard; paced asks for it anyway") {
    inputs.paced_by_default = false;
    CHECK(ChooseMode(inputs).mode == PacingMode::kStandard);
    inputs.request = PacingRequest::kPaced;
    CHECK(ChooseMode(inputs).mode == PacingMode::kPaced);
  }
  SECTION("standard is always Standard") {
    inputs.request = PacingRequest::kStandard;
    CHECK(ChooseMode(inputs).mode == PacingMode::kStandard);
  }
}

TEST_CASE("VRR needs tearing; without the patch it still runs", "[pacer]") {
  ModeInputs inputs;
  inputs.request = PacingRequest::kVrr;
  inputs.paced_by_default = true;
  inputs.caps.tearing = true;  // what the unpatched SDL reports
  CHECK(ChooseMode(inputs).mode == PacingMode::kVrr);
  inputs.caps.tearing = false;
  const ModeChoice choice = ChooseMode(inputs);
  CHECK(choice.mode == PacingMode::kStandard);
  CHECK(choice.reason.find("IMMEDIATE") != std::string_view::npos);
}

TEST_CASE("The 0.5% rule", "[pacer]") {
  CHECK(PeriodsAgree(k60 * 1.0049, k60));
  CHECK(PeriodsAgree(k60 * 0.9951, k60));
  CHECK_FALSE(PeriodsAgree(k60 * 1.0051, k60));
  CHECK_FALSE(PeriodsAgree(k60 * 0.9949, k60));
  CHECK_FALSE(PeriodsAgree(0.0, k60));
  CHECK_FALSE(PeriodsAgree(k60, 0.0));
  // 59.94 against 60 is 0.1%: they agree.
  CHECK(PeriodsAgree(1.0 / 59.94, k60));
  CHECK(SamplerFixedRefresh(GoodSampler(k60)));
  CHECK_FALSE(SamplerFixedRefresh(SamplerSample{false, k60, 0.0}));
  CHECK_FALSE(SamplerFixedRefresh(SamplerSample{true, k60, 0.11 * k60}));
}

TEST_CASE("The period: the sampler, then the mode, then the measured rate",
          "[pacer]") {
  SECTION("an agreeing sampler gives its own period") {
    const PeriodChoice choice =
        ChoosePeriod(GoodSampler(k165 * 1.001), k165, 0.0);
    CHECK(choice.source == PeriodSource::kSampler);
    CHECK(choice.period_sec == Catch::Approx(k165 * 1.001));
  }
  SECTION("a disagreeing sampler gives way to the mode") {
    const PeriodChoice choice = ChoosePeriod(GoodSampler(k165), k60, k60);
    CHECK(choice.source == PeriodSource::kDisplayMode);
    CHECK(choice.period_sec == Catch::Approx(k60));
  }
  SECTION("the mode gives way to a measured rate beyond 5% (ADR-039)") {
    const PeriodChoice choice = ChoosePeriod(GoodSampler(k165), k60, k165);
    CHECK(choice.source == PeriodSource::kMeasured);
    CHECK(choice.period_sec == Catch::Approx(k165));
  }
  SECTION("without a mode the sampler is trusted") {
    CHECK(ChoosePeriod(GoodSampler(k165), 0.0, 0.0).source ==
          PeriodSource::kSampler);
  }
  SECTION("nothing known is 60 Hz") {
    const PeriodChoice choice = ChoosePeriod(SamplerSample{}, 0.0, 0.0);
    CHECK(choice.source == PeriodSource::kFallback);
    CHECK(choice.period_sec == Catch::Approx(k60));
    CHECK(ChoosePeriod(SamplerSample{}, 0.0, k165).source ==
          PeriodSource::kMeasured);
  }
  REQUIRE(PeriodSourceName(PeriodSource::kSampler) == "sampler");
}

TEST_CASE("The vblank grid", "[pacer]") {
  constexpr std::uint64_t kAnchor = 1'000'000'000;
  const double period = 6'060'606.06;
  CHECK(GridFloor(kAnchor, kAnchor, period) == kAnchor);
  CHECK(GridFloor(kAnchor + 6'000'000, kAnchor, period) == kAnchor);
  CHECK(GridFloor(kAnchor + 6'060'607, kAnchor, period) == kAnchor + 6'060'606);
  // Before the anchor.
  CHECK(GridFloor(kAnchor - 1, kAnchor, period) == kAnchor - 6'060'606);
  // A thousand periods on, without drift.
  CHECK(GridFloor(kAnchor + 6'060'606'100, kAnchor, period) ==
        kAnchor + 6'060'606'060);
  CHECK(GridNearest(kAnchor + 6'000'000, kAnchor, period) ==
        kAnchor + 6'060'606);
  CHECK(GridNearest(kAnchor + 2'000'000, kAnchor, period) == kAnchor);
}

TEST_CASE("B and the sleep target come from the costs", "[pacer]") {
  FrameCosts costs;
  costs.tick_hi = 0.4e-3;
  costs.non_tick_hi = 0.6e-3;
  costs.gpu_hi = 0.5e-3;
  // m = max(1.5 ms, 0.6 ms + 0.5 ms) = 1.5 ms.
  CHECK(PacedBudgetSec(2, costs, 0.6e-3) == Catch::Approx(3.4e-3));
  // A timer p99 of 1.2 ms makes m 1.7 ms.
  CHECK(PacedBudgetSec(2, costs, 1.2e-3) == Catch::Approx(3.6e-3));
  // Without GPU timestamps g counts as 0.
  costs.gpu_hi = kNaN;
  CHECK(PacedBudgetSec(1, costs, 0.0) == Catch::Approx(2.5e-3));

  // The stepper's B for the next frame is the same formula (capped at T).
  FixedStepper stepper(120);
  FrameClock clock;
  clock.period_sec = k60;
  clock.frame_start_ns = 1'000'000'000;
  clock.timer_p99_sec = 0.6e-3;
  costs.gpu_hi = 0.5e-3;
  costs.gpu_95 = 0.5e-3;
  costs.tick_95 = costs.tick_hi;
  costs.non_tick_95 = costs.non_tick_hi;
  static_cast<void>(stepper.Plan(clock, costs, true, false, false));
  const StepPlan next = stepper.PlanNext(clock, costs);
  REQUIRE(next.steps == 2);
  CHECK(next.budget_b == Catch::Approx(PacedBudgetSec(2, costs, 0.6e-3)));

  // The wake is the target vblank minus B.
  constexpr std::uint64_t kVblank = 2'000'000'000;
  const double period = k60 * 1e9;
  const PacedWake wake = PlanPacedWake(kVblank + 200'000, kVblank, period, 1,
                                       kVblank, next.budget_b);
  CHECK(wake.target_ns == kVblank + 16'666'667);
  CHECK(wake.wake_ns == kVblank + 16'666'667 - 3'400'000);
  REQUIRE(wake.skipped_intervals == 0);
}

TEST_CASE("The paced target keeps HalfRate's phase and skips past vblanks",
          "[pacer]") {
  constexpr std::uint64_t kAnchor = 5'000'000'000;
  const double period = 6'060'606.06;
  constexpr double kBudget = 2.0e-3;
  SECTION("first frame: one interval after the vblank at or before now") {
    const PacedWake wake =
        PlanPacedWake(kAnchor + 1'000'000, kAnchor, period, 1, 0, kBudget);
    CHECK(wake.target_ns == kAnchor + 6'060'606);
    CHECK(wake.wake_ns == kAnchor + 4'060'606);
  }
  SECTION("HalfRate targets every second vblank from the last target") {
    const std::uint64_t prev = kAnchor + 6'060'606;
    const PacedWake wake =
        PlanPacedWake(prev + 500'000, kAnchor, period, 2, prev, kBudget);
    CHECK(wake.target_ns == kAnchor + 18'181'818);
    REQUIRE(wake.skipped_intervals == 0);
  }
  SECTION("a frame later than its next target skips whole intervals") {
    const std::uint64_t prev = kAnchor;
    // 20 ms late: the vblanks at 6.06, 12.12 and 18.18 ms have passed.
    const PacedWake wake =
        PlanPacedWake(prev + 20'000'000, kAnchor, period, 1, prev, kBudget);
    CHECK(wake.target_ns == kAnchor + 24'242'424);
    REQUIRE(wake.skipped_intervals == 3);
    // No sleep: target - B has passed.
    CHECK(wake.wake_ns == prev + 22'242'424);
    const PacedWake heavy =
        PlanPacedWake(prev + 20'000'000, kAnchor, period, 1, prev, 0.006);
    CHECK(heavy.wake_ns == prev + 20'000'000);
  }
  SECTION("HalfRate skips by two so that the phase stays") {
    const std::uint64_t prev = kAnchor;
    const PacedWake wake =
        PlanPacedWake(prev + 13'000'000, kAnchor, period, 2, prev, kBudget);
    CHECK(wake.target_ns == kAnchor + 24'242'424);
    REQUIRE(wake.skipped_intervals == 2);
  }
  SECTION("a previous target off the grid snaps back onto it") {
    const std::uint64_t prev = kAnchor + 6'060'606 + 40'000;
    const PacedWake wake =
        PlanPacedWake(kAnchor + 6'500'000, kAnchor, period, 1, prev, kBudget);
    CHECK(wake.target_ns == kAnchor + 12'121'212);
  }
}

TEST_CASE("The VRR deadline: 120 Hz, 60 Hz below 120, no catch-up", "[pacer]") {
  CHECK(VrrRateHz(165.003) == 120.0);
  CHECK(VrrRateHz(120.0) == 120.0);
  CHECK(VrrRateHz(119.88) == 120.0);
  CHECK(VrrRateHz(60.0) == 60.0);
  CHECK(VrrRateHz(100.0) == 60.0);
  CHECK(VrrRateHz(0.0) == 120.0);

  constexpr std::uint64_t kPeriod = 8'333'333;
  const VrrDeadline first = PlanVrrDeadline(1'000, 0, kPeriod, 300'000);
  CHECK(first.deadline_ns == 1'000 + kPeriod);
  CHECK(first.wake_ns == 1'000 + kPeriod - 300'000);
  CHECK_FALSE(first.reanchored);
  const VrrDeadline next = PlanVrrDeadline(first.deadline_ns + 2'000'000,
                                           first.deadline_ns, kPeriod, 300'000);
  CHECK(next.deadline_ns == first.deadline_ns + kPeriod);
  // A frame that overran the next deadline starts at once and re-anchors.
  const VrrDeadline late = PlanVrrDeadline(next.deadline_ns + kPeriod + 1,
                                           next.deadline_ns, kPeriod, 300'000);
  CHECK(late.reanchored);
  CHECK(late.deadline_ns == next.deadline_ns + kPeriod + 1);
  CHECK(late.wake_ns == late.deadline_ns);
}

// A display at 100 Hz (10 ms) whose refresh 1000 is at t = 10 s.
PresentStatistics Stats(std::uint64_t shown_id, std::uint64_t shown_refresh,
                        std::uint64_t last_id) {
  PresentStatistics stats;
  stats.present_count = shown_id;
  stats.present_refresh_count = shown_refresh;
  stats.sync_refresh_count = shown_refresh;
  stats.sync_time_ns = 10'000'000'000ULL + (shown_refresh - 1000) * 10'000'000;
  stats.last_present_count = last_id;
  return stats;
}

std::uint64_t RefreshTime(std::uint64_t refresh) {
  return 10'000'000'000ULL + (refresh - 1000) * 10'000'000;
}

constexpr double kPeriod100 = 10'000'000.0;

TEST_CASE("Misses: presents on time", "[pacer]") {
  PresentTracker tracker;
  // Present i targets refresh 1000 + i and is shown there.
  for (std::uint64_t i = 1; i <= 10; ++i) {
    tracker.OnPresentIssued(i, RefreshTime(1000 + i));
    if (i >= 2) {
      const auto result =
          tracker.OnStats(Stats(i - 1, 1000 + i - 1, i), kPeriod100);
      REQUIRE(result.resolved == 1);
      REQUIRE(result.missed == 0);
    }
  }
  REQUIRE(tracker.Pending() == 1);
}

TEST_CASE("Misses: one late present", "[pacer]") {
  PresentTracker tracker;
  tracker.OnPresentIssued(1, RefreshTime(1001));
  tracker.OnPresentIssued(2, RefreshTime(1002));
  auto result = tracker.OnStats(Stats(1, 1001, 2), kPeriod100);
  REQUIRE(result.missed == 0);
  // Present 2 shows one refresh late, at 1003; present 3 is on time again
  // at 1004.
  tracker.OnPresentIssued(3, RefreshTime(1004));
  result = tracker.OnStats(Stats(2, 1003, 3), kPeriod100);
  REQUIRE(result.resolved == 1);
  REQUIRE(result.missed == 1);
  REQUIRE(result.late_refreshes == 1);
  tracker.OnPresentIssued(4, RefreshTime(1005));
  result = tracker.OnStats(Stats(3, 1004, 4), kPeriod100);
  REQUIRE(result.missed == 0);
}

TEST_CASE("Misses: a run of late presents (the one-refresh-late equilibrium)",
          "[pacer]") {
  // Every present shows one refresh after its target while the frame starts
  // stay on time: refresh deltas equal present deltas, and only the targets
  // reveal it (red team P1).
  PresentTracker tracker;
  std::uint32_t missed = 0;
  for (std::uint64_t i = 1; i <= 20; ++i) {
    tracker.OnPresentIssued(i, RefreshTime(1000 + i));
    if (i >= 2) {
      missed += tracker.OnStats(Stats(i - 1, 1000 + i, i), kPeriod100).missed;
    }
  }
  REQUIRE(missed == 19);
}

TEST_CASE("Misses: presents shown between two samples", "[pacer]") {
  PresentTracker tracker;
  for (std::uint64_t i = 1; i <= 4; ++i) {
    tracker.OnPresentIssued(i, RefreshTime(1000 + i));
  }
  REQUIRE(tracker.OnStats(Stats(1, 1001, 4), kPeriod100).missed == 0);
  // 2 and 3 were never named; 4 shows on time at 1004, so 2 and 3 were on
  // time too.
  auto result = tracker.OnStats(Stats(4, 1004, 4), kPeriod100);
  REQUIRE(result.resolved == 3);
  REQUIRE(result.missed == 0);
  // 5, 6 and 7 target 1005-1007; 7 shows at 1010, two refreshes after 6's
  // earliest possible slot: 7 is late, and 5 and 6 cannot be proven late.
  for (std::uint64_t i = 5; i <= 7; ++i) {
    tracker.OnPresentIssued(i, RefreshTime(1000 + i));
  }
  result = tracker.OnStats(Stats(7, 1010, 7), kPeriod100);
  REQUIRE(result.resolved == 3);
  REQUIRE(result.missed == 1);
  REQUIRE(result.late_refreshes == 3);
}

TEST_CASE("Misses: the statistics reset after a mode change", "[pacer]") {
  PresentTracker tracker;
  tracker.OnPresentIssued(100, RefreshTime(1001));
  tracker.OnPresentIssued(101, RefreshTime(1002));
  REQUIRE(tracker.OnStats(Stats(100, 1001, 101), kPeriod100).missed == 0);
  // The swapchain was recreated: present ids start again at 1.
  tracker.OnPresentIssued(1, RefreshTime(1010));
  auto result = tracker.OnStats(Stats(1, 1012, 1), kPeriod100);
  CHECK(result.reset);
  REQUIRE(result.missed == 0);
  REQUIRE(tracker.Pending() == 0);
  REQUIRE(tracker.Resets() == 1);
  // Tracking resumes from the new counters.
  tracker.OnPresentIssued(2, RefreshTime(1013));
  result = tracker.OnStats(Stats(2, 1013, 2), kPeriod100);
  CHECK_FALSE(result.reset);
  REQUIRE(result.resolved == 1);
  REQUIRE(result.missed == 0);
  // Counters that go backwards reset too.
  tracker.OnPresentIssued(3, RefreshTime(1014));
  result = tracker.OnStats(Stats(2, 1005, 3), kPeriod100);
  CHECK(result.reset);
  // Nothing displayed yet resolves nothing.
  REQUIRE(tracker.OnStats(PresentStatistics{}, kPeriod100).resolved == 0);
}

TEST_CASE("Misses: a full queue drops its oldest present", "[pacer]") {
  PresentTracker tracker;
  for (std::uint64_t i = 1; i <= PresentTracker::kCapacity + 3; ++i) {
    tracker.OnPresentIssued(i, RefreshTime(1000 + i));
  }
  CHECK(tracker.Pending() == PresentTracker::kCapacity);
  REQUIRE(tracker.Dropped() == 3);
}

HalfRatePolicy::FrameInput Frame165(std::uint32_t missed, double planned,
                                    double cost95) {
  HalfRatePolicy::FrameInput frame;
  frame.vblank_period_sec = k165;
  frame.frame_sec = k165;
  frame.resolved = 1;
  frame.missed = missed;
  frame.planned_full_rate_sec = planned;
  frame.full_rate_cost95_sec = cost95;
  return frame;
}

// Frames for |sec| seconds at 165 Hz; the number of frames until Active()
// changed, or -1.
int RunFor(HalfRatePolicy& policy, double sec, std::uint32_t miss_every,
           double planned, double cost95) {
  const int frames = static_cast<int>(std::ceil(sec * 165.003));
  for (int i = 0; i < frames; ++i) {
    const std::uint32_t missed =
        miss_every > 0 && i % static_cast<int>(miss_every) == 0 ? 1 : 0;
    if (policy.OnFrame(Frame165(missed, planned, cost95))) return i + 1;
  }
  return -1;
}

// The latency-wait source (ADR-066) on a 100 Hz grid whose vblank k is at
// kGrid0 + k * 10 ms: the target of a present and the wait's return.
constexpr std::uint64_t kGrid0 = 20'000'000'000ULL;
constexpr std::uint64_t kMs = 1'000'000;

std::uint64_t Vblank(std::uint64_t k) { return kGrid0 + k * 10 * kMs; }

TEST_CASE("Wait misses: a blocked wait at the target's vblank is a hit",
          "[pacer]") {
  WaitTracker tracker;
  for (std::uint64_t k = 1; k <= 10; ++k) {
    tracker.OnPresentIssued(Vblank(k));
    // The wait returns 0.4 ms after the vblank that took the present.
    const auto result =
        tracker.OnWait(true, true, Vblank(k) + 400'000, 1, kPeriod100);
    REQUIRE(result.resolved == 1);
    REQUIRE(result.missed == 0);
    REQUIRE(result.blocked);
    REQUIRE(result.phase_ns == 400'000);
  }
  REQUIRE(tracker.Pending() == 0);
  REQUIRE(MissSourceName(MissSource::kLatencyWait).find("wait") !=
          std::string_view::npos);
}

TEST_CASE("Wait misses: a return one vblank late is a miss", "[pacer]") {
  WaitTracker tracker;
  tracker.OnPresentIssued(Vblank(1));
  auto result = tracker.OnWait(true, true, Vblank(2) + 300'000, 1, kPeriod100);
  REQUIRE(result.resolved == 1);
  REQUIRE(result.missed == 1);
  REQUIRE(result.late_refreshes == 1);
  REQUIRE(result.phase_ns == 300'000);
  // Three vblanks late (a 30 ms frame), then on time again.
  tracker.OnPresentIssued(Vblank(3));
  result = tracker.OnWait(true, true, Vblank(6) + 200'000, 1, kPeriod100);
  REQUIRE(result.missed == 1);
  REQUIRE(result.late_refreshes == 3);
  tracker.OnPresentIssued(Vblank(7));
  result = tracker.OnWait(true, true, Vblank(7) + 200'000, 1, kPeriod100);
  REQUIRE(result.missed == 0);
}

TEST_CASE(
    "Wait misses: every present one late (the one-refresh-late "
    "equilibrium) counts every one",
    "[pacer]") {
  WaitTracker tracker;
  std::uint32_t missed = 0;
  for (std::uint64_t k = 1; k <= 20; ++k) {
    tracker.OnPresentIssued(Vblank(k));
    missed += tracker.OnWait(true, true, Vblank(k + 1) + 500'000, 1, kPeriod100)
                  .missed;
  }
  REQUIRE(missed == 20);
}

TEST_CASE("Wait misses: the sampler's stamp may trail the return", "[pacer]") {
  WaitTracker tracker;
  // The target is the sampler's stamp, 1 ms after the true vblank; the
  // wait returns 0.3 ms after the true vblank: still that vblank.
  tracker.OnPresentIssued(Vblank(1) + kMs);
  auto result = tracker.OnWait(true, true, Vblank(1) + 300'000, 1, kPeriod100);
  REQUIRE(result.resolved == 1);
  REQUIRE(result.missed == 0);
  REQUIRE(result.phase_ns == -700'000);
  // With the same stamp lag, a present taken one vblank later is a miss.
  tracker.OnPresentIssued(Vblank(2) + kMs);
  result = tracker.OnWait(true, true, Vblank(3) + 300'000, 1, kPeriod100);
  REQUIRE(result.missed == 1);
  // A return just before the next stamp is after the next true vblank.
  tracker.OnPresentIssued(Vblank(4));
  result = tracker.OnWait(true, true, Vblank(5) - 100'000, 1, kPeriod100);
  REQUIRE(result.missed == 1);
}

TEST_CASE("Wait misses: a wait that tells nothing never counts as a hit",
          "[pacer]") {
  WaitTracker tracker;
  SECTION("ready at once") {
    tracker.OnPresentIssued(Vblank(1));
    const auto result =
        tracker.OnWait(true, false, Vblank(1) - kMs, 1, kPeriod100);
    REQUIRE(result.resolved == 0);
    REQUIRE(result.unresolved == 1);
  }
  SECTION("a timeout") {
    tracker.OnPresentIssued(Vblank(1));
    const auto result = tracker.OnWait(false, true, Vblank(6), 1, kPeriod100);
    REQUIRE(result.resolved == 0);
    REQUIRE(result.unresolved == 1);
  }
  SECTION("no target (no vblank grid)") {
    tracker.OnPresentIssued(0);
    const auto result =
        tracker.OnWait(true, true, Vblank(1) + 400'000, 1, kPeriod100);
    REQUIRE(result.resolved == 0);
    REQUIRE(result.unresolved == 1);
  }
  SECTION("no present to wait for") {
    const auto result =
        tracker.OnWait(true, true, Vblank(1) + 400'000, 1, kPeriod100);
    REQUIRE(result.resolved == 0);
    REQUIRE(result.unresolved == 0);
  }
  REQUIRE(tracker.Pending() == 0);
}

TEST_CASE("Wait misses: latency 2 reports the present before the newest",
          "[pacer]") {
  WaitTracker tracker;
  tracker.OnPresentIssued(Vblank(1));
  // One present in the queue: the latency-2 wait is for none of them.
  auto result = tracker.OnWait(true, true, Vblank(0) + kMs, 2, kPeriod100);
  REQUIRE(result.resolved == 0);
  REQUIRE(tracker.Pending() == 1);
  tracker.OnPresentIssued(Vblank(2));
  // Present 1 was taken at vblank 2: one late.
  result = tracker.OnWait(true, true, Vblank(2) + 400'000, 2, kPeriod100);
  REQUIRE(result.resolved == 1);
  REQUIRE(result.missed == 1);
  REQUIRE(tracker.Pending() == 1);
  tracker.OnPresentIssued(Vblank(3));
  result = tracker.OnWait(true, true, Vblank(2) + 500'000, 2, kPeriod100);
  REQUIRE(result.missed == 0);
}

TEST_CASE("Wait misses: presents without a wait of their own are dropped",
          "[pacer]") {
  WaitTracker tracker;
  // Two Modal frames presented without waiting, then a frame that waits.
  tracker.OnPresentIssued(Vblank(1));
  tracker.OnPresentIssued(Vblank(2));
  tracker.OnPresentIssued(Vblank(3));
  const auto result =
      tracker.OnWait(true, true, Vblank(3) + 400'000, 1, kPeriod100);
  REQUIRE(result.unresolved == 2);
  REQUIRE(result.resolved == 1);
  REQUIRE(result.missed == 0);
  // A full queue keeps the newest presents.
  for (std::uint64_t k = 10; k < 10 + 2 * WaitTracker::kCapacity; ++k) {
    tracker.OnPresentIssued(Vblank(k));
  }
  REQUIRE(tracker.Pending() == WaitTracker::kCapacity);
}

TEST_CASE("HalfRate: entry by misses over 2 s, exit after 4 s of margin",
          "[pacer]") {
  HalfRatePolicy policy;
  // 1 miss in 50 presents (2%) for 3 s: entered once the window covers 2 s.
  const int entered = RunFor(policy, 3.0, 50, 0.004, 0.004);
  CHECK(policy.Active());
  REQUIRE(policy.PresentInterval() == 2);
  REQUIRE(entered >= 329);
  REQUIRE(entered <= 332);
  REQUIRE(policy.Entries() == 1);
  // No margin (cost 0.8 T): stays.
  REQUIRE(RunFor(policy, 5.0, 0, 0.004, 0.8 * k165) == -1);
  CHECK(policy.Active());
  // Margin for 3.9 s is not enough; a frame without margin restarts it.
  REQUIRE(RunFor(policy, 3.9, 0, 0.004, 0.5 * k165) == -1);
  REQUIRE(RunFor(policy, 0.01, 0, 0.004, 0.9 * k165) == -1);
  const int left = RunFor(policy, 5.0, 0, 0.004, 0.5 * k165);
  REQUIRE(left >= 659);
  REQUIRE(left <= 662);
  CHECK_FALSE(policy.Active());
  REQUIRE(policy.Exits() == 1);
}

TEST_CASE("HalfRate: misses below 1% never enter", "[pacer]") {
  HalfRatePolicy policy;
  REQUIRE(RunFor(policy, 10.0, 200, 0.004, 0.004) == -1);
  CHECK_FALSE(policy.Active());
  CHECK(policy.MissFraction() > 0.0);
  CHECK(policy.MissFraction() < 0.01);
}

TEST_CASE("HalfRate: entry by planned cost over the period for 2 s",
          "[pacer]") {
  HalfRatePolicy policy;
  // 1.9 s over budget, then one frame within: the timer restarts.
  REQUIRE(RunFor(policy, 1.9, 0, 1.2 * k165, 0.9 * k165) == -1);
  REQUIRE(RunFor(policy, 0.01, 0, 0.9 * k165, 0.9 * k165) == -1);
  const int entered = RunFor(policy, 3.0, 0, 1.2 * k165, 0.9 * k165);
  REQUIRE(entered >= 329);
  REQUIRE(entered <= 332);
  CHECK(policy.Active());
}

TEST_CASE("HalfRate: never at or below 100 Hz, and not counted while Modal",
          "[pacer]") {
  HalfRatePolicy policy;
  HalfRatePolicy::FrameInput frame = Frame165(1, 1.0, 1.0);
  frame.vblank_period_sec = k60;
  frame.frame_sec = k60;
  for (int i = 0; i < 600; ++i) CHECK_FALSE(policy.OnFrame(frame));
  CHECK_FALSE(policy.Active());
  frame = Frame165(1, 1.0, 1.0);
  frame.modal = true;
  for (int i = 0; i < 1000; ++i) CHECK_FALSE(policy.OnFrame(frame));
  CHECK_FALSE(policy.Active());
  // Active, then the window moves to a 60 Hz display: left at once.
  REQUIRE(RunFor(policy, 3.0, 1, 0.004, 0.004) > 0);
  CHECK(policy.Active());
  frame = Frame165(0, 0.0, 0.0);
  frame.vblank_period_sec = k60;
  CHECK(policy.OnFrame(frame));
  CHECK_FALSE(policy.Active());
}

TEST_CASE("Adaptive latency: 2 when CPU and GPU fit alone but not together",
          "[pacer]") {
  LatencyPolicy policy;
  REQUIRE(policy.Latency() == 1);
  const double t = k60;
  const double frame = k60;
  // CPU 10 ms + GPU 10 ms > 16.7 ms, each below it: 2 after 2 s.
  int changed_at = -1;
  for (int i = 0; i < 240 && changed_at < 0; ++i) {
    if (policy.OnFrame(frame, 0.010, 0.010, t, false)) changed_at = i + 1;
  }
  REQUIRE(changed_at == 120);
  REQUIRE(policy.Latency() == 2);
  // Back to 1 after 4 s in which the sum fits.
  changed_at = -1;
  for (int i = 0; i < 480 && changed_at < 0; ++i) {
    if (policy.OnFrame(frame, 0.005, 0.005, t, false)) changed_at = i + 1;
  }
  REQUIRE(changed_at == 240);
  REQUIRE(policy.Latency() == 1);
  REQUIRE(policy.Changes() == 2);

  SECTION("a CPU over the period alone does not ask for 2") {
    for (int i = 0; i < 600; ++i) {
      CHECK_FALSE(policy.OnFrame(frame, 0.020, 0.005, t, false));
    }
  }
  SECTION("an unknown GPU cost never asks for 2") {
    for (int i = 0; i < 600; ++i) {
      CHECK_FALSE(policy.OnFrame(frame, 0.012, kNaN, t, false));
    }
  }
  SECTION("Modal frames do not count") {
    for (int i = 0; i < 600; ++i) {
      CHECK_FALSE(policy.OnFrame(frame, 0.010, 0.010, t, true));
    }
  }
  SECTION("the editor's latency 2 is fixed") {
    LatencyPolicy editor(2);
    for (int i = 0; i < 600; ++i) {
      CHECK_FALSE(editor.OnFrame(frame, 0.001, 0.001, t, false));
    }
    REQUIRE(editor.Latency() == 2);
  }
}

TEST_CASE("Lateness percentiles", "[pacer]") {
  LatenessTracker tracker;
  CHECK(tracker.Percentile(0.99, 0.5e-3) == 0.5e-3);
  for (int i = 1; i <= 100; ++i) tracker.Add(i * 1e-5);
  tracker.Add(-1.0);
  REQUIRE(tracker.Count() == 101);
  CHECK(tracker.Percentile(0.0, 1.0) == 0.0);
  CHECK(tracker.Percentile(0.5, 1.0) == Catch::Approx(50e-5));
  CHECK(tracker.Percentile(0.99, 1.0) == Catch::Approx(99e-5));
  for (int i = 0; i < 1000; ++i) tracker.Add(2e-3);
  CHECK(tracker.Count() == LatenessTracker::kCapacity);
  CHECK(tracker.Percentile(0.5, 0.0) == 2e-3);
}

}  // namespace
}  // namespace se
