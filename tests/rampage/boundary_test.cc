// Copyright 2026 The simple-engine-2 Authors.
//
// R11: the clamp keeps the collider inside the arena, corners included, and
// centres a box larger than the arena on that axis. Also the legacy-ref-v1
// clamp kept for the v1 comparison, whose corner correction is undone.

#include <catch2/catch_test_macros.hpp>
#include <entt/entity/registry.hpp>

#include "game_rampage/components.h"
#include "game_rampage/systems.h"
#include "rampage/support/test_world.h"
#include "se_core/core_components.h"
#include "se_core/thread_role.h"
#include "support/test_roles.h"

namespace se::rampage {
namespace {

constexpr ArenaBounds kArena{1920.0F, 1080.0F};
constexpr Vec2 kHalf{24.0F, 24.0F};

TEST_CASE("R11: inside positions are kept", "[rampage][boundary]") {
  CHECK(ClampToArena({500.0F, 300.0F}, kHalf, kArena) == Vec2{500.0F, 300.0F});
  // Exactly touching the edges is inside.
  CHECK(ClampToArena({24.0F, 1056.0F}, kHalf, kArena) == Vec2{24.0F, 1056.0F});
}

TEST_CASE("R11: every edge and corner clamps both axes",
          "[rampage][boundary]") {
  CHECK(ClampToArena({-10.0F, 500.0F}, kHalf, kArena) == Vec2{24.0F, 500.0F});
  CHECK(ClampToArena({1910.0F, 500.0F}, kHalf, kArena) ==
        Vec2{1896.0F, 500.0F});
  CHECK(ClampToArena({500.0F, 5.0F}, kHalf, kArena) == Vec2{500.0F, 24.0F});
  CHECK(ClampToArena({500.0F, 2000.0F}, kHalf, kArena) ==
        Vec2{500.0F, 1056.0F});
  // Corners: both corrections survive.
  CHECK(ClampToArena({-50.0F, -50.0F}, kHalf, kArena) == Vec2{24.0F, 24.0F});
  CHECK(ClampToArena({5000.0F, -3.0F}, kHalf, kArena) == Vec2{1896.0F, 24.0F});
  CHECK(ClampToArena({-1.0F, 1079.0F}, kHalf, kArena) == Vec2{24.0F, 1056.0F});
  CHECK(ClampToArena({1e9F, 1e9F}, kHalf, kArena) == Vec2{1896.0F, 1056.0F});
}

TEST_CASE("R11: boxes larger than the arena are centred",
          "[rampage][boundary]") {
  // Wider than the arena: x is centred, y still clamps.
  CHECK(ClampToArena({10.0F, -100.0F}, {1000.0F, 24.0F}, kArena) ==
        Vec2{960.0F, 24.0F});
  // Larger on both axes.
  CHECK(ClampToArena({-7.0F, 9999.0F}, {2000.0F, 600.0F}, kArena) ==
        Vec2{960.0F, 540.0F});
  // Exactly the arena's size fits, at the centre.
  CHECK(ClampToArena({0.0F, 0.0F}, {960.0F, 540.0F}, kArena) ==
        Vec2{960.0F, 540.0F});
  // A half-pixel arena and a 1 px box.
  CHECK(ClampToArena({3.0F, 3.0F}, {0.5F, 0.5F}, ArenaBounds{0.5F, 2.0F}) ==
        Vec2{0.25F, 1.5F});
}

TEST_CASE("BoundarySystem clamps after movement", "[rampage][boundary]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  const entt::entity e = w.Add({1900.0F, -5.0F});
  w.Reg().emplace<KinematicBody>(e, Vec2{1200.0F, -1200.0F});
  w.Finish();
  w.Step();
  CHECK(w.Reg().get<Transform>(e).pos == Vec2{1896.0F, 24.0F});
}

TEST_CASE("legacy-ref-v1: a corner correction is undone by the other axis",
          "[rampage][boundary]") {
  Transform t;
  t.pos = {-10.0F, -10.0F};
  // Each correction rewrites both coordinates from the unclamped position,
  // so only the last one (y) survives.
  CHECK(ClampToArenaV1(t, kHalf, kArena) == Vec2{-10.0F, 0.0F});
  // Right and bottom keep a whole sprite's margin (the bounds' size against
  // the centred position).
  t.pos = {1900.0F, 500.0F};
  CHECK(ClampToArenaV1(t, kHalf, kArena) == Vec2{1872.0F, 500.0F});
  // Rotated 45 degrees the sprite's global bounds are about 67.9 px wide.
  t.rot = RotationFromHeading(45.0F);
  const Vec2 clamped = ClampToArenaV1(t, kHalf, kArena);
  CHECK(clamped.x > 1852.0F);
  CHECK(clamped.x < 1853.0F);
}

}  // namespace
}  // namespace se::rampage
