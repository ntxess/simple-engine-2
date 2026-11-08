// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/sim_clock.h"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

#include "se_core/input_frame.h"

namespace se {
namespace {

TEST_CASE("SimClock advances in fixed steps", "[sim_clock]") {
  SimClock clock;
  REQUIRE(clock.tick == 0);
  REQUIRE(clock.rate == 120);
  CHECK(clock.dt == 1.0 / 120);
  for (int i = 0; i < 120; ++i) clock.Advance();
  REQUIRE(clock.tick == 120);
  CHECK(clock.Time() == 1.0);
  clock.SetRate(60);
  CHECK(clock.dt == 1.0 / 60);
  REQUIRE(clock.tick == 120);
}

TEST_CASE("TicksFromSeconds rounds up with an integer snap", "[sim_clock]") {
  REQUIRE(TicksFromSeconds(0.0, 120) == 0);
  REQUIRE(TicksFromSeconds(-1.0, 120) == 0);
  REQUIRE(TicksFromSeconds(0.1, 120) == 12);  // 12.000000000000002
  REQUIRE(TicksFromSeconds(1.0, 120) == 120);
  REQUIRE(TicksFromSeconds(0.004, 120) == 1);  // 0.48 ticks
  REQUIRE(TicksFromSeconds(0.0084, 120) == 2);
  REQUIRE(TicksFromSeconds(5.0, 120) == 600);
  REQUIRE(TicksFromSeconds(36.0, 120) == 4320);
}

TEST_CASE("TicksFromMilliseconds is exact ceil", "[sim_clock]") {
  REQUIRE(TicksFromMilliseconds(0, 120) == 0);
  REQUIRE(TicksFromMilliseconds(100, 120) == 12);
  REQUIRE(TicksFromMilliseconds(1, 120) == 1);
  REQUIRE(TicksFromMilliseconds(8, 120) == 1);
  REQUIRE(TicksFromMilliseconds(9, 120) == 2);
  REQUIRE(TicksFromMilliseconds(5000, 120) == 600);
}

TEST_CASE("InputFrame is trivially copyable and compares by value",
          "[input_frame]") {
  static_assert(std::is_trivially_copyable_v<InputFrame>);
  InputFrame a;
  a.tick = 5;
  a.held = 0b101;
  a.pressed[2] = 1;
  a.edge_count = 1;
  a.edges[0] = InputEdge{.action = 2,
                         .down = true,
                         .stamp_ns = 99,
                         .order = 0,
                         .source = EdgeSource::kRawKeyboard};
  InputFrame b = a;
  CHECK(a == b);
  CHECK(a.Held(0));
  CHECK_FALSE(a.Held(1));
  CHECK(a.Held(2));
  CHECK_FALSE(a.Held(200));
  b.axes[3] = 0.5F;
  CHECK_FALSE(a == b);
}

}  // namespace
}  // namespace se
