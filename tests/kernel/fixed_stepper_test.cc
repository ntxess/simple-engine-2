// Copyright 2026 The simple-engine-2 Authors.
//
// FixedStepper, CostTracker and HalfRateController (se_core/time.h): the
// cadence table, ADR-008, the integer accumulator, the cap, debt and
// dilation, Overloaded, stalls, PlanNext, and the three stepper scenarios of
// the M2 exit run through tests/support/virtual_display.h.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "se_core/time.h"
#include "support/virtual_display.h"

namespace se {
namespace {

constexpr double kTick = 1.0 / 120.0;

// Ideal frames at |hz|: frame i starts at (i + 1) periods; frame 0 has no
// previous frame.
FrameClock IdealFrame(double hz, std::uint64_t index) {
  FrameClock clock;
  const double period_ns = 1e9 / hz;
  clock.frame_start_ns = static_cast<std::uint64_t>(
      std::llround(static_cast<double>(index + 1) * period_ns));
  clock.prev_frame_start_ns =
      index == 0 ? 0
                 : static_cast<std::uint64_t>(
                       std::llround(static_cast<double>(index) * period_ns));
  clock.period_sec = 1.0 / hz;
  clock.elapsed_vblanks = 1;
  clock.vblanks_known = true;
  clock.timer_p99_sec = 0.6e-3;
  return clock;
}

// Light costs that leave headroom at every rate.
FrameCosts LightCosts() {
  FrameCosts costs;
  costs.tick_hi = 0.5e-3;
  costs.non_tick_hi = 0.5e-3;
  costs.gpu_hi = 0.5e-3;
  costs.tick_95 = 0.4e-3;
  costs.non_tick_95 = 0.4e-3;
  costs.gpu_95 = 0.4e-3;
  return costs;
}

// Costs that allow one tick per 60 Hz frame (k_95 = 1, k_hi = 1).
FrameCosts HeavyCosts() {
  FrameCosts costs;
  costs.tick_hi = 8.0e-3;
  costs.non_tick_hi = 1.0e-3;
  costs.gpu_hi = 0.0;
  costs.tick_95 = 8.0e-3;
  costs.non_tick_95 = 1.0e-3;
  costs.gpu_95 = 0.0;
  return costs;
}

struct CadenceCase {
  double hz;
  Cadence cadence;
  int locked_k;
  int k_rt;
};

TEST_CASE("Cadence and k_rt at common display rates", "[stepper]") {
  const std::vector<CadenceCase> cases = {
      {30.0, Cadence::kLocked, 4, 4},  {50.0, Cadence::kFree, 0, 3},
      {60.0, Cadence::kLocked, 2, 2},  {59.94, Cadence::kLocked, 2, 2},
      {120.0, Cadence::kLocked, 1, 1}, {119.88, Cadence::kLocked, 1, 1},
      {144.0, Cadence::kFree, 0, 1},   {160.0, Cadence::kFree, 0, 1},
      {165.0, Cadence::kFree, 0, 1},   {240.0, Cadence::kLocked, 1, 1},
  };
  for (const CadenceCase& c : cases) {
    INFO("hz = " << c.hz);
    FixedStepper stepper;
    const StepPlan plan =
        stepper.Plan(IdealFrame(c.hz, 0), LightCosts(), true, false, false);
    CHECK(plan.cadence == c.cadence);
    CHECK(plan.locked_k == c.locked_k);
    CHECK(plan.k_rt == c.k_rt);
  }
}

TEST_CASE("k_rt is the locked ratio at 59.94 Hz (ADR-008)", "[stepper]") {
  FixedStepper stepper;
  // ceil(T/dt) would be ceil(2.002) = 3.
  REQUIRE(std::ceil((1.0 / 59.94) / kTick) == 3.0);
  for (std::uint64_t i = 0; i < 600; ++i) {
    const StepPlan plan =
        stepper.Plan(IdealFrame(59.94, i), LightCosts(), true, false, false);
    REQUIRE(plan.k_rt == 2);
    REQUIRE(plan.steps == 2);
    REQUIRE(plan.alpha == 1.0F);
  }
  // The 0.1% residue is counted as dilation: 600 frames * (T - 2 dt).
  const double expected = 600 * (1.0 / 59.94 - 2 * kTick);
  CHECK(std::abs(stepper.Stats().residue_sec - expected) < 1e-9);
}

struct TickCountCase {
  double hz;
  int frames;
  std::uint64_t ticks;
};

TEST_CASE("Ticks per frame follow the cadence with no load", "[stepper]") {
  // 10 s at each rate. Free cadence snaps deltas to the period, so 144 Hz
  // runs 5 ticks per 6 frames and 165 Hz 8 per 11.
  const std::vector<TickCountCase> cases = {
      {30.0, 300, 1200},   {60.0, 600, 1200},   {120.0, 1200, 1200},
      {144.0, 1440, 1200}, {160.0, 1600, 1200}, {165.0, 1650, 1200},
  };
  for (const TickCountCase& c : cases) {
    INFO("hz = " << c.hz);
    FixedStepper stepper;
    std::vector<int> steps;
    std::uint64_t total = 0;
    for (int i = 0; i < c.frames; ++i) {
      const StepPlan plan =
          stepper.Plan(IdealFrame(c.hz, static_cast<std::uint64_t>(i)),
                       LightCosts(), true, false, false);
      steps.push_back(plan.steps);
      total += static_cast<std::uint64_t>(plan.steps);
      if (plan.cadence == Cadence::kFree) {
        REQUIRE(plan.alpha >= 0.0F);
        REQUIRE(plan.alpha < 1.0F);
      }
    }
    // The last tick may still sit in the accumulator by rounding.
    CHECK(total + 1 >= c.ticks);
    CHECK(total <= c.ticks);
    // After k frames exactly floor(k T / dt) ticks have run, give or take
    // the one in the accumulator: 5 per 6 frames at 144 Hz, 8 per 11 at
    // 165 Hz, 3 per 4 at 160 Hz, and no drift.
    const double per_frame = 120.0 / c.hz;
    std::uint64_t cumulative = 0;
    for (std::size_t k = 0; k < steps.size(); ++k) {
      cumulative += static_cast<std::uint64_t>(steps[k]);
      const double ideal = static_cast<double>(k + 1) * per_frame;
      REQUIRE(static_cast<double>(cumulative) <= ideal + 1e-6);
      REQUIRE(static_cast<double>(cumulative) > ideal - 1.0 - 1e-6);
    }
  }
}

TEST_CASE("The accumulator is exact: 120 ticks per simulated second",
          "[stepper]") {
  // Free cadence (144 Hz) with deltas of 2 and 3 ms, which never snap:
  // 240,000 frames, exactly 600 s of wall time.
  FixedStepper stepper;
  FrameClock clock;
  clock.period_sec = 1.0 / 144.0;
  clock.vblanks_known = true;
  clock.timer_p99_sec = 0.0;
  std::uint64_t now = 1'000'000;
  std::uint64_t ticks = 0;
  // The first frame only establishes the start time.
  clock.frame_start_ns = now;
  clock.prev_frame_start_ns = 0;
  static_cast<void>(stepper.Plan(clock, LightCosts(), false, false, false));
  for (int i = 0; i < 240000; ++i) {
    clock.prev_frame_start_ns = now;
    now += (i % 2 == 0) ? 2'000'000 : 3'000'000;
    clock.frame_start_ns = now;
    ticks += static_cast<std::uint64_t>(
        stepper.Plan(clock, LightCosts(), true, false, false).steps);
  }
  REQUIRE(ticks == 72000);
  REQUIRE(stepper.AccumulatorUnits() == 0);
  REQUIRE(stepper.Stats().dilated_ticks == 0);
}

TEST_CASE("The cap formulas", "[stepper]") {
  FixedStepper stepper;
  SECTION("Normal k_max is clamp(max(k_rt, k_hi), 1, min(8, k_rt + 1))") {
    // 60 Hz with light costs: k_hi is large, so k_max = k_rt + 1 = 3.
    StepPlan plan =
        stepper.Plan(IdealFrame(60.0, 0), LightCosts(), true, false, false);
    REQUIRE(plan.k_max == 3);
    // Heavy costs: k_hi = floor((16.67 - 1 - 0 - 1.5) / 8) = 1 < k_rt.
    plan = stepper.Plan(IdealFrame(60.0, 1), HeavyCosts(), true, false, false);
    REQUIRE(plan.k_max == 2);
    // 30 Hz: k_rt = 4, k_max = 5.
    FixedStepper slow;
    plan = slow.Plan(IdealFrame(30.0, 0), LightCosts(), true, false, false);
    REQUIRE(plan.k_max == 5);
  }
  SECTION("No measurements yet allow k_rt + 1") {
    const StepPlan plan =
        stepper.Plan(IdealFrame(144.0, 0), FrameCosts{}, true, false, false);
    REQUIRE(plan.k_max == 2);
  }
  SECTION("144 Hz with 2 ticks due runs both when the budget allows") {
    static_cast<void>(
        stepper.Plan(IdealFrame(144.0, 0), LightCosts(), true, false, false));
    FrameClock late = IdealFrame(144.0, 1);
    late.frame_start_ns += 10'000'000;  // 17 ms after the previous start
    const StepPlan plan = stepper.Plan(late, LightCosts(), true, false, false);
    REQUIRE(plan.k_max == 2);
    REQUIRE(plan.steps == 2);
    // With N=50k costs only one tick fits a 144 Hz frame.
    FixedStepper heavy;
    FrameCosts n50k;
    n50k.tick_hi = 5.0e-3;
    n50k.non_tick_hi = 1.4e-3;
    n50k.tick_95 = 4.6e-3;
    n50k.non_tick_95 = 1.4e-3;
    n50k.gpu_hi = 0.0;
    n50k.gpu_95 = 0.0;
    static_cast<void>(
        heavy.Plan(IdealFrame(144.0, 0), n50k, true, false, false));
    const StepPlan capped = heavy.Plan(late, n50k, true, false, false);
    REQUIRE(capped.k_max == 1);
    REQUIRE(capped.steps == 1);
  }
  SECTION("Unknown GPU time with the fence pending holds k_max at k_rt") {
    FrameCosts unknown_gpu = LightCosts();
    unknown_gpu.gpu_hi = std::numeric_limits<double>::quiet_NaN();
    unknown_gpu.gpu_95 = std::numeric_limits<double>::quiet_NaN();
    FrameClock clock = IdealFrame(60.0, 0);
    clock.gpu_fence_pending_at_start = true;
    REQUIRE(stepper.Plan(clock, unknown_gpu, true, false, false).k_max == 2);
    clock.gpu_fence_pending_at_start = false;
    REQUIRE(stepper.Plan(clock, unknown_gpu, true, false, false).k_max == 3);
    clock.gpu_fence_pending_at_start = true;
    REQUIRE(stepper.Plan(clock, LightCosts(), true, false, false).k_max == 3);
  }
}

TEST_CASE("Budget B and the miss offset", "[stepper]") {
  FixedStepper stepper;
  const FrameCosts costs = LightCosts();
  const double m = SafetyMarginSec(0.6e-3, 0.0);
  REQUIRE(m == 1.5e-3);
  REQUIRE(std::abs(SafetyMarginSec(1.5e-3, 0.0) - 2.0e-3) < 1e-15);
  REQUIRE(std::abs(SafetyMarginSec(0.0, 1e-3) - 2.5e-3) < 1e-15);
  StepPlan plan = stepper.Plan(IdealFrame(60.0, 0), costs, true, false, false);
  REQUIRE(plan.steps == 2);
  const double expected =
      2 * costs.tick_hi + costs.non_tick_hi + costs.gpu_hi + m;
  CHECK(std::abs(plan.budget_b - expected) < 1e-12);

  // A miss adds 1 ms, decaying by 0.05 ms per frame.
  stepper.OnPresentResult(true, false);
  plan = stepper.Plan(IdealFrame(60.0, 1), costs, true, false, false);
  CHECK(std::abs(plan.budget_b - (expected + 1e-3)) < 1e-12);
  plan = stepper.Plan(IdealFrame(60.0, 2), costs, true, false, false);
  CHECK(std::abs(plan.budget_b - (expected + 0.95e-3)) < 1e-12);
  // Misses while Modal do not count.
  stepper.OnPresentResult(true, true);
  plan = stepper.Plan(IdealFrame(60.0, 3), costs, true, false, false);
  CHECK(std::abs(plan.budget_b - (expected + 0.90e-3)) < 1e-12);

  // B never exceeds T plus the offset.
  FixedStepper heavy;
  FrameCosts huge = HeavyCosts();
  huge.tick_hi = 20e-3;
  plan = heavy.Plan(IdealFrame(60.0, 0), huge, true, false, false);
  CHECK(plan.budget_b == 1.0 / 60.0);
}

TEST_CASE("Tick input windows", "[stepper]") {
  SECTION("Locked cadence splits the frame evenly") {
    FixedStepper stepper;
    static_cast<void>(
        stepper.Plan(IdealFrame(60.0, 0), LightCosts(), true, false, false));
    const FrameClock clock = IdealFrame(60.0, 1);
    const StepPlan plan = stepper.Plan(clock, LightCosts(), true, false, false);
    REQUIRE(plan.steps == 2);
    const std::uint64_t span = clock.frame_start_ns - clock.prev_frame_start_ns;
    CHECK(plan.tick_window_end_ns[0] == clock.prev_frame_start_ns + span / 2);
    CHECK(plan.tick_window_end_ns[1] == clock.frame_start_ns);
  }
  SECTION("Free cadence ends tick i at start - remainder - (n-1-i) dt") {
    FixedStepper stepper;
    FrameClock clock = IdealFrame(144.0, 0);
    clock.frame_start_ns = 1'000'000'000;
    static_cast<void>(stepper.Plan(clock, LightCosts(), true, false, false));
    clock.prev_frame_start_ns = clock.frame_start_ns;
    clock.frame_start_ns += 17'000'000;  // not near a multiple of T
    const StepPlan plan = stepper.Plan(clock, LightCosts(), true, false, false);
    REQUIRE(plan.steps == 2);
    const std::uint64_t remainder = stepper.AccumulatorUnits() / 120;
    CHECK(plan.tick_window_end_ns[1] == clock.frame_start_ns - remainder);
    CHECK(plan.tick_window_end_ns[0] ==
          clock.frame_start_ns - remainder - 1'000'000'000ULL / 120);
  }
}

TEST_CASE("Stalls are paused time with no catch-up", "[stepper]") {
  for (const double hz : {60.0, 144.0}) {
    INFO("hz = " << hz);
    FixedStepper stepper;
    for (std::uint64_t i = 0; i < 10; ++i) {
      static_cast<void>(
          stepper.Plan(IdealFrame(hz, i), LightCosts(), true, false, false));
    }
    FrameClock stall = IdealFrame(hz, 10);
    stall.frame_start_ns = stall.prev_frame_start_ns + 300'000'000;
    stall.elapsed_vblanks = static_cast<std::uint32_t>(0.3 * hz);
    const StepPlan plan = stepper.Plan(stall, LightCosts(), true, false, false);
    CHECK(plan.steps <= plan.k_rt);
    REQUIRE(stepper.Stats().stalls == 1);
    CHECK(std::abs(stepper.Stats().paused_sec - (0.3 - 1.0 / hz)) < 1e-6);
    REQUIRE(stepper.Stats().dilated_ticks == 0);
  }
}

TEST_CASE("A debugger lowers the stall threshold to 100 ms", "[stepper]") {
  auto run = [](bool debugger) {
    FixedStepper stepper;
    static_cast<void>(
        stepper.Plan(IdealFrame(60.0, 0), LightCosts(), true, debugger, false));
    FrameClock pause = IdealFrame(60.0, 1);
    pause.frame_start_ns = pause.prev_frame_start_ns + 200'000'000;
    pause.elapsed_vblanks = 12;
    const StepPlan plan =
        stepper.Plan(pause, LightCosts(), true, debugger, false);
    return std::make_pair(plan, stepper.Stats());
  };
  const auto [with_plan, with_stats] = run(true);
  REQUIRE(with_stats.stalls == 1);
  REQUIRE(with_plan.steps == 2);
  REQUIRE(with_stats.dilated_ticks == 0);

  const auto [without_plan, without_stats] = run(false);
  REQUIRE(without_stats.stalls == 0);
  // 24 ticks due, k_max 3: one spare tick per frame repays 15 in 0.25 s,
  // so 15 stay owed and the other 6 are dilated.
  REQUIRE(without_plan.steps == 3);
  REQUIRE(without_plan.debt_ticks == 15);
  REQUIRE(without_stats.dilated_ticks == 6);
}

TEST_CASE("Debt beyond 0.25 s of spare rate becomes counted dilation",
          "[stepper]") {
  SECTION("Free cadence keeps repayable debt and dilates the rest") {
    FixedStepper stepper;
    // 144 Hz with N=50k costs: k_max = 1, spare = 1 - 0.833 = 0.167 ticks
    // per frame, so 0.25 s repays floor(0.167 * 36) = 6 ticks.
    FrameCosts costs;
    costs.tick_hi = 5.0e-3;
    costs.non_tick_hi = 1.4e-3;
    costs.tick_95 = 4.0e-3;
    costs.non_tick_95 = 1.0e-3;
    costs.gpu_hi = 0.0;
    costs.gpu_95 = 0.0;
    FrameClock clock = IdealFrame(144.0, 0);
    static_cast<void>(stepper.Plan(clock, costs, true, false, false));
    clock.prev_frame_start_ns = clock.frame_start_ns;
    clock.frame_start_ns += 100'000'000;  // 100 ms: 12 ticks due
    const StepPlan plan = stepper.Plan(clock, costs, true, false, false);
    REQUIRE(plan.k_max == 1);
    REQUIRE(plan.steps == 1);
    REQUIRE(plan.debt_ticks == 6);
    REQUIRE(stepper.Stats().dilated_ticks == 5);
    CHECK(std::abs(plan.dilation_sec - 5 * kTick) < 1e-12);
    // The fraction is kept: the debt drains one extra tick per frame
    // (spare rate) and alpha stays 1 until it is repaid.
    CHECK(plan.alpha == 1.0F);
    std::uint64_t ticks = 0;
    for (int i = 0; i < 36; ++i) {
      clock.prev_frame_start_ns = clock.frame_start_ns;
      clock.frame_start_ns += 6'944'444;
      ticks += static_cast<std::uint64_t>(
          stepper.Plan(clock, costs, true, false, false).steps);
    }
    REQUIRE(ticks == 36);
    REQUIRE(stepper.Stats().dilated_ticks == 5);
  }
  SECTION("Locked 60 Hz has no spare rate at k_max = k_rt") {
    FixedStepper stepper;
    static_cast<void>(
        stepper.Plan(IdealFrame(60.0, 0), HeavyCosts(), true, false, false));
    FrameClock late = IdealFrame(60.0, 1);
    late.elapsed_vblanks = 3;  // two missed vblanks: 6 ticks due
    const StepPlan plan = stepper.Plan(late, HeavyCosts(), true, false, false);
    REQUIRE(plan.k_max == 2);
    REQUIRE(plan.steps == 2);
    REQUIRE(plan.debt_ticks == 0);
    REQUIRE(stepper.Stats().dilated_ticks == 4);
  }
}

TEST_CASE("Overloaded entry and exit", "[stepper]") {
  SECTION("Two consecutive misses with debt enter Overloaded") {
    FixedStepper stepper;
    static_cast<void>(
        stepper.Plan(IdealFrame(144.0, 0), LightCosts(), true, false, false));
    FrameClock late = IdealFrame(144.0, 1);
    late.frame_start_ns += 30'000'000;
    const StepPlan plan = stepper.Plan(late, LightCosts(), true, false, false);
    REQUIRE(plan.debt_ticks > 0);
    stepper.OnPresentResult(true, false);
    CHECK_FALSE(stepper.Overloaded());
    stepper.OnPresentResult(true, false);
    CHECK(stepper.Overloaded());
    REQUIRE(stepper.Stats().overload_entries == 1);
  }
  SECTION("Misses without debt or while Modal do not") {
    FixedStepper stepper;
    static_cast<void>(
        stepper.Plan(IdealFrame(60.0, 0), LightCosts(), true, false, false));
    stepper.OnPresentResult(true, false);
    stepper.OnPresentResult(true, false);
    CHECK_FALSE(stepper.Overloaded());
    FrameClock late = IdealFrame(144.0, 1);
    FixedStepper modal;
    static_cast<void>(
        modal.Plan(IdealFrame(144.0, 0), LightCosts(), true, false, false));
    late.frame_start_ns += 30'000'000;
    static_cast<void>(modal.Plan(late, LightCosts(), true, false, false));
    modal.OnPresentResult(true, true);
    modal.OnPresentResult(true, true);
    CHECK_FALSE(modal.Overloaded());
  }
  SECTION("k_95 below k_rt for 1 s enters; 1 s clean with k_95 >= k_rt exits") {
    FixedStepper stepper;
    std::uint64_t frame = 0;
    for (; frame < 59; ++frame) {
      static_cast<void>(stepper.Plan(IdealFrame(60.0, frame), HeavyCosts(),
                                     true, false, false));
    }
    CHECK_FALSE(stepper.Overloaded());
    for (; frame < 62; ++frame) {
      static_cast<void>(stepper.Plan(IdealFrame(60.0, frame), HeavyCosts(),
                                     true, false, false));
    }
    REQUIRE(stepper.Overloaded());
    StepPlan plan = stepper.Plan(IdealFrame(60.0, frame++), HeavyCosts(), true,
                                 false, false);
    CHECK(plan.overloaded);
    REQUIRE(plan.k_max == 1);  // clamp(k_95 = 1, 1, k_rt = 2)

    // Costs recover; a miss half-way restarts the clean second.
    for (int i = 0; i < 30; ++i) {
      static_cast<void>(stepper.Plan(IdealFrame(60.0, frame++), LightCosts(),
                                     true, false, false));
    }
    stepper.OnPresentResult(true, false);
    for (int i = 0; i < 59; ++i) {
      static_cast<void>(stepper.Plan(IdealFrame(60.0, frame++), LightCosts(),
                                     true, false, false));
    }
    CHECK(stepper.Overloaded());
    for (int i = 0; i < 3; ++i) {
      static_cast<void>(stepper.Plan(IdealFrame(60.0, frame++), LightCosts(),
                                     true, false, false));
    }
    CHECK_FALSE(stepper.Overloaded());
    REQUIRE(stepper.Stats().overload_exits == 1);
  }
  SECTION("k_95 below k_rt while Modal never enters") {
    FixedStepper stepper;
    for (std::uint64_t frame = 0; frame < 240; ++frame) {
      FrameClock clock = IdealFrame(60.0, frame);
      clock.modal = true;
      static_cast<void>(stepper.Plan(clock, HeavyCosts(), true, false, false));
    }
    CHECK_FALSE(stepper.Overloaded());
  }
}

TEST_CASE("Editor Play floors Overloaded at 0 ticks", "[stepper]") {
  FrameCosts crushing = HeavyCosts();
  crushing.tick_95 = 20e-3;  // k_95 = 0 at 60 Hz
  crushing.tick_hi = 20e-3;
  for (const bool editor : {true, false}) {
    INFO("editor_play = " << editor);
    FixedStepper stepper;
    StepPlan plan;
    for (std::uint64_t frame = 0; frame < 70; ++frame) {
      plan =
          stepper.Plan(IdealFrame(60.0, frame), crushing, true, false, editor);
    }
    REQUIRE(plan.overloaded);
    CHECK(plan.k_max == (editor ? 0 : 1));
    CHECK(plan.steps == (editor ? 0 : 1));
    // The simulation dilates; no tick is skipped.
    REQUIRE(stepper.Stats().dilated_ticks > 0);
  }
}

TEST_CASE("A paused simulation runs no ticks and accumulates nothing",
          "[stepper]") {
  FixedStepper stepper;
  for (std::uint64_t frame = 0; frame < 60; ++frame) {
    const StepPlan plan = stepper.Plan(IdealFrame(144.0, frame), LightCosts(),
                                       false, false, false);
    REQUIRE(plan.steps == 0);
    REQUIRE(plan.alpha == 1.0F);
  }
  REQUIRE(stepper.Stats().ticks == 0);
  const StepPlan resumed =
      stepper.Plan(IdealFrame(144.0, 60), LightCosts(), true, false, false);
  REQUIRE(resumed.steps <= 1);
}

TEST_CASE("PlanNext predicts the next on-time frame before the sleep",
          "[stepper]") {
  for (const double hz : {60.0, 59.94, 144.0, 165.0}) {
    INFO("hz = " << hz);
    FixedStepper stepper;
    for (std::uint64_t frame = 0; frame < 300; ++frame) {
      const FrameClock clock = IdealFrame(hz, frame);
      static_cast<void>(stepper.Plan(clock, LightCosts(), true, false, false));
      const StepPlan predicted = stepper.PlanNext(clock, LightCosts());
      // PlanNext does not change the stepper.
      const StepPlan again = stepper.PlanNext(clock, LightCosts());
      REQUIRE(again.steps == predicted.steps);
      REQUIRE(again.budget_b == predicted.budget_b);
      // The real next frame, arriving on time, gets the predicted plan.
      FixedStepper copy = stepper;
      const StepPlan actual = copy.Plan(IdealFrame(hz, frame + 1), LightCosts(),
                                        true, false, false);
      REQUIRE(actual.steps == predicted.steps);
      REQUIRE(actual.budget_b == predicted.budget_b);
    }
  }
}

TEST_CASE("ResetAccumulator clears debt and remainder", "[stepper]") {
  FixedStepper stepper;
  static_cast<void>(
      stepper.Plan(IdealFrame(144.0, 0), LightCosts(), true, false, false));
  FrameClock late = IdealFrame(144.0, 1);
  late.frame_start_ns += 5'000'000;
  static_cast<void>(stepper.Plan(late, LightCosts(), true, false, false));
  REQUIRE(stepper.AccumulatorUnits() > 0);
  stepper.ResetAccumulator();
  REQUIRE(stepper.AccumulatorUnits() == 0);
}

TEST_CASE("CostTracker keeps p99.5 over 4 s and p95 over 2 s", "[stepper]") {
  CostTracker tracker;
  // 4 s at 60 Hz, 2 ticks per frame: 480 ticks of 4 ms and two 25 ms
  // spikes. p99.5 of 480 values is the 478th smallest, so the two spikes
  // do not raise it.
  for (int frame = 0; frame < 240; ++frame) {
    tracker.AddTick(4e-3);
    tracker.AddTick(frame == 60 || frame == 180 ? 25e-3 : 4e-3);
    tracker.AddFrame(1.0 / 60.0, 1.4e-3,
                     std::numeric_limits<double>::quiet_NaN());
  }
  FrameCosts costs = tracker.Current();
  CHECK(costs.tick_hi == 4e-3);
  CHECK(costs.tick_95 == 4e-3);
  CHECK(costs.non_tick_hi == 1.4e-3);
  CHECK(std::isnan(costs.gpu_hi));
  CHECK(std::isnan(costs.gpu_95));
  CHECK(tracker.TickPercentile(1.0, 4.0, 0.0) == 25e-3);

  // A third spike inside the window is the 478th value: p99.5 rises.
  tracker.AddTick(25e-3);
  tracker.AddFrame(1.0 / 60.0, 1.4e-3, 0.5e-3);
  costs = tracker.Current();
  CHECK(costs.tick_hi == 25e-3);
  CHECK(costs.gpu_hi == 0.5e-3);

  // Old samples leave the window.
  for (int frame = 0; frame < 300; ++frame) {
    tracker.AddTick(1e-3);
    tracker.AddFrame(1.0 / 60.0, 1e-3, 0.5e-3);
  }
  costs = tracker.Current();
  CHECK(costs.tick_hi == 1e-3);
  CHECK(costs.non_tick_95 == 1e-3);
  tracker.Reset();
  CHECK(tracker.Current().tick_hi == 0.0);
}

TEST_CASE("HalfRate enters above 1% misses over 2 s, above 100 Hz",
          "[stepper][half_rate]") {
  constexpr double kPeriod = 1.0 / 144.0;
  SECTION("3 misses per 288 frames (1.04%) enter once 2 s are seen") {
    HalfRateController controller;
    int entered = -1;
    for (int frame = 0; frame < 400 && entered < 0; ++frame) {
      controller.OnFrame(kPeriod, kPeriod, frame % 96 == 5, false, 1.0);
      if (controller.Active()) entered = frame;
    }
    REQUIRE(entered >= 286);
    REQUIRE(entered <= 290);
    REQUIRE(controller.PresentInterval() == 2);
    REQUIRE(controller.Entries() == 1);
  }
  SECTION("2 misses per 300 frames (0.67%) never enter") {
    HalfRateController controller;
    for (int frame = 0; frame < 15000; ++frame) {
      controller.OnFrame(kPeriod, kPeriod, frame % 150 == 5, false, 1.0);
    }
    CHECK_FALSE(controller.Active());
    REQUIRE(controller.Entries() == 0);
  }
  SECTION("never at 100 Hz or below, and Modal frames do not count") {
    HalfRateController slow;
    HalfRateController modal;
    for (int frame = 0; frame < 1200; ++frame) {
      slow.OnFrame(1.0 / 100.0, 1.0 / 100.0, true, false, 1.0);
      modal.OnFrame(kPeriod, kPeriod, true, true, 1.0);
    }
    CHECK_FALSE(slow.Active());
    CHECK_FALSE(modal.Active());
  }
  SECTION("leaves after 5 s of headroom, not before") {
    HalfRateController controller;
    for (int frame = 0; frame < 300; ++frame) {
      controller.OnFrame(kPeriod, kPeriod, true, false, 1.0);
    }
    REQUIRE(controller.Active());
    const double fits = 0.7 * kPeriod;
    const double tight = 0.8 * kPeriod;
    for (int frame = 0; frame < 300; ++frame) {
      controller.OnFrame(kPeriod, 2 * kPeriod, false, false, fits);
    }
    // A tight frame restarts the 5 s; 5 s at 72 Hz is 360 frames.
    controller.OnFrame(kPeriod, 2 * kPeriod, false, false, tight);
    REQUIRE(controller.Active());
    int frames = 0;
    while (controller.Active() && frames < 1000) {
      controller.OnFrame(kPeriod, 2 * kPeriod, false, false, fits);
      ++frames;
    }
    REQUIRE(frames >= 360);
    REQUIRE(frames <= 361);
    REQUIRE(controller.Exits() == 1);
    // Misses at the halved rate do not re-enter it immediately.
    REQUIRE(controller.Entries() == 1);
  }
  SECTION("the full-rate cost estimate") {
    FrameCosts costs;
    costs.tick_95 = 4.6e-3;
    costs.non_tick_95 = 1.4e-3;
    costs.gpu_95 = std::numeric_limits<double>::quiet_NaN();
    const double cost =
        HalfRateController::FullRateCost95(costs, kPeriod, kTick, 1.5e-3);
    CHECK(std::abs(cost - (kPeriod / kTick * 4.6e-3 + 1.4e-3 + 1.5e-3)) <
          1e-15);
  }
}

// The M2 exit scenarios.

TEST_CASE("N=50k at 60 Hz with a 25 ms spike: at most 1 hitch per spike",
          "[stepper][scenario][slow]") {
  testing::CostModel model = testing::CostsN50k();
  model.spike_sec = 25e-3;
  model.spike_period_sec = 2.0;
  for (const bool paced : {true, false}) {
    INFO("paced = " << paced);
    testing::FixedStepperPolicy policy;
    const testing::RunResult run = testing::RunVirtualDisplay(
        {.hz = 60.0, .paced = paced}, model, policy, 20.0);
    REQUIRE(run.spikes >= 9);
    REQUIRE(run.MaxLateFramesPerSpike(1.9) <= 1);
    CHECK(run.late_frames <= run.spikes);
    REQUIRE(run.max_steps <= 3);
    // No lock-in: almost every frame presents on time.
    REQUIRE(run.frames.size() >= 1150);
  }
}

TEST_CASE("The old 8-step cap reproduces the lock-in after a spike",
          "[stepper][scenario][slow]") {
  testing::CostModel model = testing::CostsN50k();
  model.spike_sec = 25e-3;
  model.spike_period_sec = 2.0;
  testing::AccumulatorCapPolicy old_loop(8, 0.0);
  const testing::RunResult run = testing::RunVirtualDisplay(
      {.hz = 60.0, .paced = false}, model, old_loop, 20.0);
  // After the first spike every frame owes 4 ticks (18.2 ms > 16.7 ms) and
  // misses: the loop locks at 30 Hz (the rig measured 115 hitches).
  REQUIRE(run.LateFramesAfter(2.5) >= 100);
  CHECK(run.LateFramesAfter(2.5) * 10 >=
        static_cast<std::uint32_t>(run.frames.size()) * 8 / 2);
  REQUIRE(run.FramesWithSteps(4) >= 100);
}

TEST_CASE("59.94 Hz Locked at N=30k for 60 s has no 3-tick frames",
          "[stepper][scenario][slow]") {
  const testing::CostModel model = testing::CostsN30k();
  testing::FixedStepperPolicy policy;
  const testing::RunResult run = testing::RunVirtualDisplay(
      {.hz = 59.94, .paced = true}, model, policy, 60.0);
  REQUIRE(run.frames.size() >= 3590);
  REQUIRE(run.FramesWithSteps(3) == 0);
  REQUIRE(run.max_steps == 2);
  REQUIRE(run.late_frames == 0);
  for (const testing::FrameRecord& f : run.frames) {
    REQUIRE(f.cadence == Cadence::kLocked);
    REQUIRE(f.k_rt == 2);
  }
  CHECK(run.ticks == 2 * run.frames.size());
  // The red team's fixed late latch (8.38 ms, at most 3 steps) on a free
  // accumulator drifts into 3-tick frames at 59.94 Hz.
  testing::AccumulatorCapPolicy latch(3, 8.38e-3);
  const testing::RunResult drift = testing::RunVirtualDisplay(
      {.hz = 59.94, .paced = true}, model, latch, 60.0);
  REQUIRE(drift.FramesWithSteps(3) >= 1);
}

TEST_CASE("N=50k at 144 Hz settles in HalfRate", "[stepper][scenario][slow]") {
  testing::CostModel model = testing::CostsN50k();
  // Tick-plus-render variance near 80% load: a 1.5 ms preemption on 2% of
  // frames (the red team measured 17 and 5 hitches per 5 s).
  model.preempt_probability = 0.02;
  model.preempt_sec = 1.5e-3;
  testing::FixedStepperPolicy policy;
  HalfRateController half_rate;
  const testing::RunResult run = testing::RunVirtualDisplay(
      {.hz = 144.0, .paced = true}, model, policy, 20.0, &half_rate);
  CHECK(half_rate.Active());
  REQUIRE(half_rate.Entries() == 1);
  REQUIRE(half_rate.Exits() == 0);
  // Entered within the first 5 s, then 72 Hz without hitches.
  std::uint64_t entered_ns = 0;
  for (const testing::FrameRecord& f : run.frames) {
    if (f.present_interval == 2) {
      entered_ns = f.start_ns;
      break;
    }
  }
  REQUIRE(entered_ns > 0);
  REQUIRE(entered_ns < 5'000'000'000ULL);
  REQUIRE(run.LateFramesAfter(10.0) == 0);
  REQUIRE(run.frames.back().present_interval == 2);
}

}  // namespace
}  // namespace se
