// Copyright 2026 The simple-engine-2 Authors.
//
// Path following: R6 (zero-length segments), R12 (steps follow the
// polyline: several waypoints in one step, the exact stop at the end, loops
// far shorter than the step, and the end of a step that a round no longer
// shortens) and R13 (the closing segment of repeating paths, two points,
// coincident points), plus the PathSystem's Speed binding and Moved flag,
// and the legacy-ref-v1 behaviour kept for the v1 comparison.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <entt/entity/registry.hpp>
#include <limits>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/systems.h"
#include "rampage/support/test_world.h"
#include "se_core/thread_role.h"
#include "support/test_roles.h"

namespace se::rampage {
namespace {

using testing::RampageTestWorld;

struct Fixture {
  Path path;
  PathCache cache;
};

Fixture Make(std::vector<Vec2> points, bool loop) {
  Fixture f;
  f.path.points = std::move(points);
  f.path.loop = loop;
  BuildPathCache(f.path, f.cache);
  return f;
}

// The heading in degrees of the direction (dx, dy), normalised to [0, 360).
float Heading(const PathStep& step) {
  return std::fmod(step.heading + 360.0F, 360.0F);
}

TEST_CASE("Path caches hold segment lengths and running totals",
          "[rampage][path]") {
  const Fixture open = Make({{0, 0}, {3, 4}, {3, 4}, {3, 10}}, false);
  CHECK(open.cache.lengths == std::vector<float>{5.0F, 0.0F, 6.0F});
  CHECK(open.cache.totals == std::vector<float>{0.0F, 5.0F, 5.0F, 11.0F});
  REQUIRE(SegmentCount(open.path) == 3);
  const Fixture loop = Make({{0, 0}, {3, 4}, {3, 0}}, true);
  CHECK(loop.cache.lengths == std::vector<float>{5.0F, 4.0F, 3.0F});
  REQUIRE(SegmentCount(loop.path) == 3);
  REQUIRE(SegmentCount(Make({{1, 1}}, true).path) == 0);
}

TEST_CASE("R6: zero-length segments are skipped within the step",
          "[rampage][path]") {
  Fixture f = Make({{0, 0}, {0, 0}, {0, 0}, {10, 0}}, false);
  const PathStep step = AdvancePath(f.path, f.cache, 4.0F);
  REQUIRE(step.active);
  CHECK(step.pos == Vec2{4.0F, 0.0F});
  REQUIRE(f.path.segment == 2);
  CHECK(f.path.u == 4.0F);
  // Moving along +x: atan2(0, 10) = 0, plus 90 degrees.
  CHECK(Heading(step) == 90.0F);
}

TEST_CASE("R12: one step passes several waypoints", "[rampage][path]") {
  Fixture f = Make({{0, 0}, {1, 0}, {1, 1}, {2, 1}, {2, 2}, {10, 2}}, false);
  const PathStep step = AdvancePath(f.path, f.cache, 4.5F);
  REQUIRE(step.active);
  REQUIRE(f.path.segment == 4);
  CHECK(f.path.u == 0.5F);
  CHECK(step.pos == Vec2{2.5F, 2.0F});
  CHECK(Heading(step) == 90.0F);
  CHECK_FALSE(f.path.done);
}

TEST_CASE("R12: a step ending exactly on a waypoint continues from it",
          "[rampage][path]") {
  Fixture f = Make({{0, 0}, {4, 0}, {4, 4}}, false);
  const PathStep step = AdvancePath(f.path, f.cache, 4.0F);
  REQUIRE(f.path.segment == 1);
  CHECK(f.path.u == 0.0F);
  CHECK(step.pos == Vec2{4.0F, 0.0F});
  // On the new segment, heading down (+y): atan2(4, 0) = 90, plus 90.
  CHECK(Heading(step) == 180.0F);
}

TEST_CASE("R12: the end of a path is exact and final", "[rampage][path]") {
  Fixture f = Make({{0, 0}, {4, 0}, {4, 0}}, false);
  PathStep step = AdvancePath(f.path, f.cache, 2.0F);
  CHECK(step.pos == Vec2{2.0F, 0.0F});
  step = AdvancePath(f.path, f.cache, 3.0F);
  REQUIRE(step.active);
  CHECK(step.pos == Vec2{4.0F, 0.0F});
  CHECK(f.path.done);
  REQUIRE(step.has_heading);
  CHECK(Heading(step) == 90.0F);
  // Done for good.
  CHECK_FALSE(AdvancePath(f.path, f.cache, 2.0F).active);

  // A path whose points all coincide ends on its point at once.
  Fixture still = Make({{5, 5}, {5, 5}}, false);
  step = AdvancePath(still.path, still.cache, 1.0F);
  REQUIRE(step.active);
  CHECK(step.pos == Vec2{5.0F, 5.0F});
  CHECK_FALSE(step.has_heading);
  CHECK(still.path.done);
}

TEST_CASE("R13: repeating paths close the loop", "[rampage][path]") {
  Fixture f = Make({{0, 0}, {10, 0}, {10, 10}}, true);
  PathStep step = AdvancePath(f.path, f.cache, 25.0F);
  // 10 along the first segment, 10 down the second, 5 along the closing
  // segment back to the start.
  REQUIRE(f.path.segment == 2);
  CHECK(f.path.u == 5.0F);
  const float len = std::sqrt(200.0F);
  CHECK(step.pos == Vec2{10.0F + (0.0F - 10.0F) * (5.0F / len),
                         10.0F + (0.0F - 10.0F) * (5.0F / len)});
  // Heading up and left: atan2(-10, -10) = -135, plus 90.
  CHECK(std::abs(Heading(step) - 315.0F) < 1e-4F);
  step = AdvancePath(f.path, f.cache, 10.0F);
  // Past the first point, onto segment 0 again.
  REQUIRE(f.path.segment == 0);
  CHECK(f.path.u == (5.0F + 10.0F) - len);
  CHECK(step.pos.y == 0.0F);
  CHECK_FALSE(f.path.done);
}

TEST_CASE("R13: a two-point loop goes back and forth", "[rampage][path]") {
  Fixture f = Make({{0, 0}, {10, 0}}, true);
  PathStep step = AdvancePath(f.path, f.cache, 15.0F);
  REQUIRE(f.path.segment == 1);
  CHECK(step.pos == Vec2{5.0F, 0.0F});
  CHECK(Heading(step) == 270.0F);
  step = AdvancePath(f.path, f.cache, 10.0F);
  REQUIRE(f.path.segment == 0);
  CHECK(step.pos == Vec2{5.0F, 0.0F});
  CHECK(Heading(step) == 90.0F);
}

TEST_CASE("R13: a loop of coincident points never moves", "[rampage][path]") {
  Fixture f = Make({{7, 7}, {7, 7}, {7, 7}}, true);
  CHECK_FALSE(AdvancePath(f.path, f.cache, 5.0F).active);
  Fixture single = Make({{7, 7}}, true);
  CHECK_FALSE(AdvancePath(single.path, single.cache, 5.0F).active);
  // A zero-length closing segment is skipped like any other.
  Fixture closed = Make({{0, 0}, {10, 0}, {0, 0}}, true);
  const PathStep step = AdvancePath(closed.path, closed.cache, 25.0F);
  REQUIRE(closed.path.segment == 0);
  CHECK(step.pos == Vec2{5.0F, 0.0F});
}

TEST_CASE("R12: a loop far shorter than the step goes round until it is used",
          "[rampage][path]") {
  // 2e7 px/s on a 1 px loop: the step loses exactly 1 per segment, over
  // 166666 segments in one tick, and ends 0.671875 into segment 0.
  Fixture probe = Make({{500, 500}, {501, 500}}, true);
  const float fast = 2.0e7F * (1.0F / 120.0F);
  REQUIRE(fast == 166666.671875F);
  PathStep step = AdvancePath(probe.path, probe.cache, fast);
  REQUIRE(probe.path.segment == 0);
  CHECK(probe.path.u == 0.671875F);
  CHECK(step.pos == Vec2{500.671875F, 500.0F});
  CHECK(Heading(step) == 90.0F);

  // 80000 half-pixel segments in one step.
  Fixture half = Make({{0, 0}, {0.5F, 0}}, true);
  step = AdvancePath(half.path, half.cache, 40000.25F);
  REQUIRE(half.path.segment == 0);
  CHECK(half.path.u == 0.25F);
  CHECK(step.pos == Vec2{0.25F, 0.0F});
}

TEST_CASE("R12: a sub-pixel loop at the stress speed stays on its points",
          "[rampage][path]") {
  // Segments of 2^-14 px (one float step at x = 900) at 500 px/s: 68266
  // segments per tick, each subtracted exactly.
  const float x1 = 900.00006F;
  Fixture f = Make({{900, 300}, {x1, 300}}, true);
  const float len = x1 - 900.0F;
  REQUIRE(f.cache.lengths == std::vector<float>{len, len});
  const float rem = 500.0F * (1.0F / 120.0F);
  PathStep step = AdvancePath(f.path, f.cache, rem);
  REQUIRE(f.path.segment == 0);
  CHECK(f.path.u == rem - 68266.0F * len);
  CHECK(step.pos == Vec2{x1, 300.0F});
  for (int tick = 0; tick < 50; ++tick) {
    step = AdvancePath(f.path, f.cache, rem);
    INFO("tick " << tick);
    REQUIRE(step.active);
    CHECK(step.pos.x >= 900.0F);
    CHECK(step.pos.x <= x1);
    CHECK(step.pos.y == 300.0F);
    CHECK(f.path.u < f.cache.lengths[f.path.segment]);
    CHECK_FALSE(f.path.done);
  }
}

TEST_CASE("R12: a step that a round no longer shortens ends at the start",
          "[rampage][path]") {
  // 1e6 px on a loop of 2^-13 px segments: the subtractions vanish in
  // rounding, so the second wrap ends the step on the first point.
  Fixture f = Make({{1000, 0}, {1000.0001F, 0}}, true);
  REQUIRE(f.cache.lengths[0] > 0.0F);
  PathStep step = AdvancePath(f.path, f.cache, 1.0e6F);
  REQUIRE(f.path.segment == 0);
  CHECK(f.path.u == 0.0F);
  CHECK(step.pos == Vec2{1000.0F, 0.0F});
  CHECK(Heading(step) == 90.0F);

  // So do NaN and infinite steps.
  for (const float bad : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity()}) {
    Fixture loop = Make({{0, 0}, {10, 0}, {10, 10}}, true);
    loop.path.segment = 1;
    loop.path.u = 3.0F;
    step = AdvancePath(loop.path, loop.cache, bad);
    REQUIRE(loop.path.segment == 0);
    CHECK(loop.path.u == 0.0F);
    CHECK(step.pos == Vec2{0.0F, 0.0F});
    CHECK(Heading(step) == 90.0F);
    // A non-repeating path ends on its last point.
    Fixture open = Make({{0, 0}, {4, 0}, {4, 4}}, false);
    step = AdvancePath(open.path, open.cache, bad);
    CHECK(open.path.done);
    CHECK(step.pos == Vec2{4.0F, 4.0F});
    CHECK(Heading(step) == 180.0F);
  }
}

TEST_CASE("A path whose segment is out of range starts over",
          "[rampage][path]") {
  Fixture f = Make({{0, 0}, {4, 0}, {4, 4}}, false);
  f.path.segment = 7;
  f.path.u = 3.0F;
  const PathStep step = AdvancePath(f.path, f.cache, 1.0F);
  REQUIRE(f.path.segment == 0);
  CHECK(f.path.u == 1.0F);
  CHECK(step.pos == Vec2{1.0F, 0.0F});
  CHECK_FALSE(f.path.done);
}

entt::entity Mover(RampageTestWorld& w, std::vector<Vec2> points, bool loop,
                   float speed) SE_REQUIRES(g_sim_owner) {
  const entt::entity e = w.Add(points.front(), {24.0F, 24.0F});
  Path path;
  path.points = std::move(points);
  path.loop = loop;
  w.Reg().emplace<Path>(e, std::move(path));
  // HP keeps it alive (R10: Stats without HP die at the end of the tick).
  w.SetStat(e, "HP", 100.0F);
  if (speed >= 0.0F) w.SetStat(e, "Speed", speed);
  return e;
}

TEST_CASE("PathSystem moves at the Speed stat and flags moves",
          "[rampage][path]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  // 120 px/s = 1 px per tick.
  const entt::entity a = Mover(w, {{100, 100}, {400, 100}}, false, 120.0F);
  const entt::entity none = Mover(w, {{100, 300}, {400, 300}}, false, -1.0F);
  const entt::entity still = Mover(w, {{100, 500}, {400, 500}}, false, 0.0F);
  w.Finish();
  w.Step();
  CHECK(w.Reg().get<Transform>(a).pos.x == 101.0F);
  // The heading (90 degrees) in sf::Angle's radians.
  CHECK(w.Reg().get<Transform>(a).rot == RotationFromHeading(90.0F));
  // Without a Speed stat nothing happens; at speed 0 it does not move.
  CHECK(w.Reg().get<Transform>(none).pos.x == 100.0F);
  CHECK(w.Reg().get<Transform>(still).pos.x == 100.0F);
  // Moved is set within the tick and cleared by Cleanup.
  CHECK(w.Reg().view<Moved>().empty());
  w.Step(298);
  CHECK(w.Reg().get<Transform>(a).pos.x == 399.0F);
  w.Step(5);
  CHECK(w.Reg().get<Transform>(a).pos.x == 400.0F);
  CHECK(w.Reg().get<Path>(a).done);
}

TEST_CASE("legacy-ref-v1 paths overshoot corners and replay offset",
          "[rampage][path]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  SceneSettings settings;
  settings.rules = RulesVersion::kV1;
  RampageTestWorld w(settings);
  // 600 px/s = 5 px per tick; the corner at x = 412 is overshot.
  const entt::entity e =
      Mover(w, {{400, 100}, {412, 100}, {412, 200}}, true, 600.0F);
  w.Finish();
  w.Step(3);
  // The whole step follows the first segment past its end: 415.
  CHECK(w.Reg().get<Transform>(e).pos == Vec2{415.0F, 100.0F});
  w.Step();
  CHECK(w.Reg().get<Transform>(e).pos == Vec2{415.0F, 105.0F});
}

}  // namespace
}  // namespace se::rampage
