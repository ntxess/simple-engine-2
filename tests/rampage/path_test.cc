// Copyright 2026 The simple-engine-2 Authors.
//
// Path following: R6 (zero-length segments), R12 (steps follow the
// polyline: several waypoints in one step, the exact stop at the end) and
// R13 (the closing segment of repeating paths, two points, coincident
// points), plus the PathSystem's Speed binding and Moved flag, and the
// legacy-ref-v1 behaviour kept for the v1 comparison.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <entt/entity/registry.hpp>
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
