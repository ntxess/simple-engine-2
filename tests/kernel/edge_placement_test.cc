// Copyright 2026 The simple-engine-2 Authors.
//
// PlaceEdges (se_core/edge_placement.h) on synthetic frames: the M8 rule
// that puts each stamped input edge on the tick whose window holds it, with
// the windows FixedStepper::Plan computes (Locked: equal splits of the
// frame; Free: start - remainder - (steps - 1 - i) dt), at 30, 60, 144 and
// 165 Hz, the last-tick rule and frames without a tick.

#include "se_core/edge_placement.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "se_core/time.h"

namespace se {
namespace {

constexpr std::uint64_t kMs = 1'000'000;
constexpr std::uint64_t kTickNs = 8'333'333;

FrameClock IdealFrame(double hz, std::uint64_t index, std::uint64_t base_ns) {
  FrameClock clock;
  const double period_ns = 1e9 / hz;
  clock.frame_start_ns =
      base_ns + static_cast<std::uint64_t>(
                    std::llround(static_cast<double>(index + 1) * period_ns));
  clock.prev_frame_start_ns =
      index == 0 ? 0
                 : base_ns + static_cast<std::uint64_t>(std::llround(
                                 static_cast<double>(index) * period_ns));
  clock.period_sec = 1.0 / hz;
  clock.elapsed_vblanks = 1;
  clock.vblanks_known = true;
  clock.timer_p99_sec = 0.6e-3;
  return clock;
}

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

std::span<const std::uint64_t> Windows(const StepPlan& plan) {
  return {plan.tick_window_end_ns.data(), static_cast<std::size_t>(plan.steps)};
}

TEST_CASE("PlaceEdge takes the first window that ends after the stamp",
          "[edge_placement]") {
  const std::array<std::uint64_t, 3> ends = {100, 200, 300};
  REQUIRE(PlaceEdge(0, ends) == 0);
  REQUIRE(PlaceEdge(99, ends) == 0);
  // Half-open windows: the end belongs to the next tick.
  REQUIRE(PlaceEdge(100, ends) == 1);
  REQUIRE(PlaceEdge(299, ends) == 2);
  // After the last window: the last tick.
  REQUIRE(PlaceEdge(300, ends) == 2);
  REQUIRE(PlaceEdge(10'000, ends) == 2);
  // No tick: the edge waits.
  REQUIRE(PlaceEdge(50, std::span<const std::uint64_t>()) == kEdgeWaits);

  const std::array<std::uint64_t, 5> stamps = {350, 5, 150, 100, 299};
  std::array<int, 5> out{};
  PlaceEdges(stamps, ends, out);
  REQUIRE(out == std::array<int, 5>{2, 0, 1, 1, 2});
  REQUIRE(EdgeBelongsToTick(99, 100, false));
  CHECK_FALSE(EdgeBelongsToTick(100, 100, false));
  REQUIRE(EdgeBelongsToTick(100, 100, true));
}

TEST_CASE("The window helpers are the design's formulas", "[edge_placement]") {
  std::array<std::uint64_t, 4> locked{};
  LockedTickWindows(1000, 1000 + 33'333'333, locked);
  REQUIRE(locked[0] == 1000 + 33'333'333 / 4);
  REQUIRE(locked[1] == 1000 + 33'333'333 * 2 / 4);
  REQUIRE(locked[3] == 1000 + 33'333'333);

  std::array<std::uint64_t, 3> free{};
  FreeTickWindows(100 * kMs, 3 * kMs, 120, free);
  REQUIRE(free[2] == 97 * kMs);
  REQUIRE(free[1] == 97 * kMs - 1'000'000'000ULL / 120);
  REQUIRE(free[0] == 97 * kMs - 2'000'000'000ULL / 120);
  // Clamped at 0, never wrapped.
  std::array<std::uint64_t, 2> early{};
  FreeTickWindows(5 * kMs, 1 * kMs, 120, early);
  REQUIRE(early[0] == 0);
  REQUIRE(early[1] == 4 * kMs);
}

TEST_CASE("Locked cadence: presses 8 ms apart land on different ticks",
          "[edge_placement]") {
  for (const double hz : {30.0, 60.0}) {
    FixedStepper stepper;
    const std::uint64_t base = 5'000'000'000;
    static_cast<void>(stepper.Plan(IdealFrame(hz, 0, base), LightCosts(), true,
                                   false, false));
    const FrameClock clock = IdealFrame(hz, 1, base);
    const StepPlan plan = stepper.Plan(clock, LightCosts(), true, false, false);
    REQUIRE(plan.cadence == Cadence::kLocked);
    const int k = hz == 30.0 ? 4 : 2;
    REQUIRE(plan.steps == k);
    // The stepper's windows are the equal splits of [prev_start, start).
    std::array<std::uint64_t, kMaxStepsPerFrame> expected{};
    LockedTickWindows(
        clock.prev_frame_start_ns, clock.frame_start_ns,
        std::span<std::uint64_t>(expected.data(), static_cast<std::size_t>(k)));
    for (int i = 0; i < k; ++i) {
      REQUIRE(plan.tick_window_end_ns[static_cast<std::size_t>(i)] ==
              expected[static_cast<std::size_t>(i)]);
    }
    // Presses 8 ms apart from 1 ms into the frame: one per tick.
    const std::uint64_t prev = clock.prev_frame_start_ns;
    for (int i = 0; i < k; ++i) {
      const std::uint64_t stamp =
          prev + 1 * kMs + static_cast<std::uint64_t>(i) * 8 * kMs;
      REQUIRE(PlaceEdge(stamp, Windows(plan)) == i);
    }
    // Before the frame's span: the first tick; after it: the last.
    REQUIRE(PlaceEdge(prev - 3 * kMs, Windows(plan)) == 0);
    REQUIRE(PlaceEdge(clock.frame_start_ns + 2 * kMs, Windows(plan)) == k - 1);
  }
}

TEST_CASE("Free cadence: windows end one tick apart before the remainder",
          "[edge_placement]") {
  for (const double hz : {144.0, 165.0}) {
    FixedStepper stepper;
    const std::uint64_t base = 2'000'000'000;
    int frames_with_two = 0;
    for (std::uint64_t f = 0; f < 600; ++f) {
      const FrameClock clock = IdealFrame(hz, f, base);
      const StepPlan plan =
          stepper.Plan(clock, LightCosts(), true, false, false);
      REQUIRE(plan.cadence == Cadence::kFree);
      if (plan.steps == 0) continue;
      const std::uint64_t remainder = stepper.AccumulatorUnits() / 120;
      const std::size_t last = static_cast<std::size_t>(plan.steps) - 1;
      REQUIRE(plan.tick_window_end_ns[last] ==
              clock.frame_start_ns - remainder);
      for (std::size_t i = 1; i <= last; ++i) {
        REQUIRE(plan.tick_window_end_ns[i] - plan.tick_window_end_ns[i - 1] ==
                1'000'000'000ULL / 120);
      }
      if (plan.steps > 1) ++frames_with_two;
    }
    // Neither rate needs two ticks in one ideal frame.
    REQUIRE(frames_with_two == 0);
  }
}

// Frames at |hz| with one edge stamped in every frame interval: each edge
// is "pumped" by the frame that starts after it and placed there, or waits
// through frames without a tick. Returns the most frames any edge waited.
int MostFramesWaited(double hz, int* zero_tick_frames) {
  FixedStepper stepper;
  const std::uint64_t base = 3'000'000'000;
  std::vector<std::uint64_t> waiting;
  std::vector<int> waited;
  int most = 0;
  *zero_tick_frames = 0;
  for (std::uint64_t f = 1; f < 1200; ++f) {
    const FrameClock clock = IdealFrame(hz, f, base);
    // An edge stamped 1 ms before this frame's start arrived meanwhile.
    waiting.push_back(clock.frame_start_ns - 1 * kMs);
    waited.push_back(0);
    const StepPlan plan = stepper.Plan(clock, LightCosts(), true, false, false);
    if (plan.steps == 0) {
      ++*zero_tick_frames;
      for (std::size_t i = 0; i < waiting.size(); ++i) {
        REQUIRE(PlaceEdge(waiting[i], Windows(plan)) == kEdgeWaits);
        ++waited[i];
      }
      continue;
    }
    for (std::size_t i = 0; i < waiting.size(); ++i) {
      const int tick = PlaceEdge(waiting[i], Windows(plan));
      REQUIRE(tick >= 0);
      REQUIRE(tick < plan.steps);
      if (waited[i] > most) most = waited[i];
    }
    waiting.clear();
    waited.clear();
  }
  return most;
}

TEST_CASE("Frames without a tick hold their edges for the next tick only",
          "[edge_placement]") {
  int zero_144 = 0;
  REQUIRE(MostFramesWaited(144.0, &zero_144) == 1);
  // One frame in six has no tick at 144 Hz (120 / 144).
  REQUIRE(zero_144 >= 195);
  REQUIRE(zero_144 <= 205);
  int zero_165 = 0;
  REQUIRE(MostFramesWaited(165.0, &zero_165) == 1);
  REQUIRE(zero_165 >= 320);
  // At 60 Hz Locked every frame has two ticks: nothing waits.
  int zero_60 = 0;
  REQUIRE(MostFramesWaited(60.0, &zero_60) == 0);
  REQUIRE(zero_60 == 0);
}

TEST_CASE("Placement keeps stamp order across a frame's ticks",
          "[edge_placement]") {
  FixedStepper stepper;
  const std::uint64_t base = 1'000'000'000;
  static_cast<void>(stepper.Plan(IdealFrame(30.0, 0, base), LightCosts(), true,
                                 false, false));
  const FrameClock clock = IdealFrame(30.0, 1, base);
  const StepPlan plan = stepper.Plan(clock, LightCosts(), true, false, false);
  REQUIRE(plan.steps == 4);
  int previous = 0;
  for (std::uint64_t stamp = clock.prev_frame_start_ns - kTickNs;
       stamp < clock.frame_start_ns + kTickNs; stamp += 250'000) {
    const int tick = PlaceEdge(stamp, Windows(plan));
    REQUIRE(tick >= previous);
    previous = tick;
  }
  REQUIRE(previous == 3);
}

}  // namespace
}  // namespace se
