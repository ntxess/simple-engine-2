// Copyright 2026 The simple-engine-2 Authors.
//
// BuildRenderList interpolation: PrevTransform -> Transform by alpha, the
// shorter arc for rotation, and the cases that draw the Transform as it is
// (teleports, stale or missing PrevTransform, spawns).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <limits>

#include "render/render_fixture.h"
#include "se_core/core_components.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"
#include "support/test_roles.h"

namespace se {
namespace {

using Catch::Matchers::WithinAbs;

constexpr float kEps = 1e-3F;

Vec2 Center(const SpriteQuad& quad) {
  Vec2 sum;
  for (const Vec2& p : quad.pos) sum = sum + p;
  return sum * 0.25F;
}

void CheckQuadsMatch(const SpriteQuad& a, const SpriteQuad& b) {
  for (int i = 0; i < 4; ++i) {
    CHECK_THAT(a.pos[i].x, WithinAbs(b.pos[i].x, kEps));
    CHECK_THAT(a.pos[i].y, WithinAbs(b.pos[i].y, kEps));
  }
}

// The single quad of a one-sprite World.
SpriteQuad Build(const World& world, float alpha) {
  RenderList list;
  BuildRenderList(world, testing::WorldCamera(1000.0F, 1000.0F), alpha, list);
  REQUIRE(list.quads.size() == 1);
  return list.quads[0];
}

TEST_CASE("Alpha 0, 0.5 and 1 interpolate position and scale linearly",
          "[render][interpolation]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  const entt::entity e = testing::AddSprite(
      world, 1, {100.0F, 100.0F}, testing::CenteredSprite(1, 10.0F, 10.0F));
  testing::StepTick(world);
  Transform& t = testing::MutableTransform(world, e);
  t.pos = {200.0F, 300.0F};
  t.scale = {3.0F, 1.0F};

  RenderList list;
  const Camera2D camera = testing::WorldCamera(1000.0F, 1000.0F);
  BuildRenderList(world, camera, 0.0F, list);
  REQUIRE(list.quads.size() == 1);
  REQUIRE(list.stats.interpolated == 1);
  // Exact at alpha 0: the previous tick's pose.
  CHECK(list.quads[0].pos[0] == Vec2{95.0F, 95.0F});
  CHECK(list.quads[0].pos[2] == Vec2{105.0F, 105.0F});

  BuildRenderList(world, camera, 0.5F, list);
  CHECK(list.quads[0].pos[0] == Vec2{140.0F, 195.0F});
  CHECK(list.quads[0].pos[2] == Vec2{160.0F, 205.0F});

  // Exact at alpha 1 (Locked cadence): the Transform.
  BuildRenderList(world, camera, 1.0F, list);
  CHECK(list.quads[0].pos[0] == Vec2{185.0F, 295.0F});
  CHECK(list.quads[0].pos[2] == Vec2{215.0F, 305.0F});
}

TEST_CASE("Rotation takes the shorter arc across +-180 degrees",
          "[render][interpolation]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());

  // In degrees; Transform::rot holds radians.
  struct Case {
    float from;
    float to;
    float alpha;
    float expected;
  };
  const Case cases[] = {
      {170.0F, -170.0F, 0.5F, 180.0F},  // through 180, not through 0
      {-170.0F, 170.0F, 0.5F, 180.0F},
      {170.0F, -170.0F, 0.25F, 175.0F},
      {10.0F, 350.0F, 0.5F, 0.0F},  // through 0, not through 180
      {350.0F, 10.0F, 0.75F, 5.0F},
      {720.0F + 90.0F, 100.0F, 0.5F, 95.0F},  // whole turns do not count
      // A half turn is ambiguous; it goes the negative way.
      {0.0F, 180.0F, 0.5F, -90.0F},
      {0.0F, -180.0F, 0.5F, -90.0F},
  };
  for (const Case& c : cases) {
    INFO("from " << c.from << " to " << c.to << " at " << c.alpha);
    World world(testing::RenderRegistry(), {});
    const SpriteRenderer sprite = testing::CenteredSprite(1, 40.0F, 10.0F);
    const entt::entity e = testing::AddSprite(world, 1, {500.0F, 500.0F},
                                              sprite, testing::Degrees(c.from));
    testing::StepTick(world);
    testing::MutableTransform(world, e).rot = testing::Degrees(c.to);
    const SpriteQuad moving = Build(world, c.alpha);

    World reference(testing::RenderRegistry(), {});
    static_cast<void>(testing::AddSprite(reference, 1, {500.0F, 500.0F}, sprite,
                                         testing::Degrees(c.expected)));
    CheckQuadsMatch(moving, Build(reference, 1.0F));
  }
}

TEST_CASE("A teleport is drawn at the Transform until the next tick",
          "[render][interpolation]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  const entt::entity e = testing::AddSprite(
      world, 1, {100.0F, 100.0F}, testing::CenteredSprite(1, 10.0F, 10.0F));
  testing::StepTick(world);
  Transform& t = testing::MutableTransform(world, e);
  t.pos = {800.0F, 800.0F};
  t.teleport_tick = world.Clock().tick;

  RenderList list;
  const Camera2D camera = testing::WorldCamera(1000.0F, 1000.0F);
  BuildRenderList(world, camera, 0.5F, list);
  REQUIRE(list.quads.size() == 1);
  CHECK(Center(list.quads[0]) == Vec2{800.0F, 800.0F});
  REQUIRE(list.stats.interpolated == 0);

  // The next tick interpolates again, from the teleport's destination.
  testing::StepTick(world);
  testing::MutableTransform(world, e).pos = {810.0F, 820.0F};
  BuildRenderList(world, camera, 0.5F, list);
  CHECK(Center(list.quads[0]) == Vec2{805.0F, 810.0F});
  REQUIRE(list.stats.interpolated == 1);
}

TEST_CASE("A stale or missing PrevTransform draws the Transform",
          "[render][interpolation]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  const entt::entity e = testing::AddSprite(
      world, 1, {100.0F, 100.0F}, testing::CenteredSprite(1, 10.0F, 10.0F));
  testing::StepTick(world);
  testing::StepTick(world);
  testing::MutableTransform(world, e).pos = {300.0F, 100.0F};
  RenderList list;
  const Camera2D camera = testing::WorldCamera(1000.0F, 1000.0F);

  SECTION("from an older tick") {
    world.Reg().get<PrevTransform>(e).tick = world.Clock().tick - 2;
    BuildRenderList(world, camera, 0.5F, list);
    REQUIRE(list.quads.size() == 1);
    CHECK(Center(list.quads[0]) == Vec2{300.0F, 100.0F});
    REQUIRE(list.stats.interpolated == 0);
  }
  SECTION("from the current tick") {
    world.Reg().get<PrevTransform>(e).tick = world.Clock().tick;
    BuildRenderList(world, camera, 0.5F, list);
    REQUIRE(list.quads.size() == 1);
    CHECK(Center(list.quads[0]) == Vec2{300.0F, 100.0F});
    REQUIRE(list.stats.interpolated == 0);
  }
  SECTION("missing") {
    world.Reg().remove<PrevTransform>(e);
    BuildRenderList(world, camera, 0.5F, list);
    REQUIRE(list.quads.size() == 1);
    CHECK(Center(list.quads[0]) == Vec2{300.0F, 100.0F});
    REQUIRE(list.stats.interpolated == 0);
  }
}

TEST_CASE("A spawn is drawn where it spawned, then interpolates",
          "[render][interpolation]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  testing::StepTick(world);
  testing::StepTick(world);
  // Spawned at the current tick: FinalizeSpawns sets teleport_tick to it.
  const entt::entity e = testing::AddSprite(
      world, 1, {100.0F, 100.0F}, testing::CenteredSprite(1, 10.0F, 10.0F));
  REQUIRE(world.CReg().get<Transform>(e).teleport_tick == world.Clock().tick);
  RenderList list;
  const Camera2D camera = testing::WorldCamera(1000.0F, 1000.0F);
  BuildRenderList(world, camera, 0.25F, list);
  REQUIRE(list.quads.size() == 1);
  CHECK(Center(list.quads[0]) == Vec2{100.0F, 100.0F});
  REQUIRE(list.stats.interpolated == 0);

  testing::StepTick(world);
  testing::MutableTransform(world, e).pos = {140.0F, 100.0F};
  BuildRenderList(world, camera, 0.25F, list);
  CHECK(Center(list.quads[0]) == Vec2{110.0F, 100.0F});
  REQUIRE(list.stats.interpolated == 1);
}

TEST_CASE("Alpha is clamped to [0, 1] and NaN draws the Transform",
          "[render][interpolation]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  const entt::entity e = testing::AddSprite(
      world, 1, {100.0F, 100.0F}, testing::CenteredSprite(1, 10.0F, 10.0F));
  testing::StepTick(world);
  testing::MutableTransform(world, e).pos = {200.0F, 100.0F};

  CHECK(Center(Build(world, -1.0F)) == Vec2{100.0F, 100.0F});
  CHECK(Center(Build(world, 2.0F)) == Vec2{200.0F, 100.0F});
  CHECK(Center(Build(world, std::numeric_limits<float>::quiet_NaN())) ==
        Vec2{200.0F, 100.0F});
}

}  // namespace
}  // namespace se
