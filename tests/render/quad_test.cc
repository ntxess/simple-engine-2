// Copyright 2026 The simple-engine-2 Authors.
//
// SpriteQuad geometry: the origin as the pivot of position, rotation and
// scale; flips mirror about the origin; texture coordinates and tint.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <entt/entity/registry.hpp>

#include "render/render_fixture.h"
#include "se_core/core_components.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"
#include "support/test_roles.h"

namespace se {
namespace {

using Catch::Matchers::WithinAbs;

// An 8 x 4 region at texel (16, 32) of texture 7, pivoting about (2, 1).
SpriteRenderer OffCentre() {
  SpriteRenderer sprite;
  sprite.texture = 7;
  sprite.region = {16.0F, 32.0F, 8.0F, 4.0F};
  sprite.origin = {2.0F, 1.0F};
  sprite.tint = {10, 20, 30, 40};
  return sprite;
}

SpriteQuad BuildOne(const SpriteRenderer& sprite, float rot, Vec2 scale)
    SE_REQUIRES(g_main) {
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  const entt::entity e =
      testing::AddSprite(world, 1, {10.0F, 20.0F}, sprite, rot);
  testing::MutableTransform(world, e).scale = scale;
  RenderList list;
  BuildRenderList(world, testing::WorldCamera(100.0F, 100.0F), 1.0F, list);
  REQUIRE(list.quads.size() == 1);
  REQUIRE(list.batches.size() == 1);
  REQUIRE(list.batches[0].texture == 7);
  return list.quads[0];
}

void CheckPos(const SpriteQuad& quad, int corner, float x, float y) {
  INFO("corner " << corner);
  CHECK_THAT(quad.pos[corner].x, WithinAbs(x, 1e-4));
  CHECK_THAT(quad.pos[corner].y, WithinAbs(y, 1e-4));
}

TEST_CASE("The origin is drawn at the Transform position", "[render][quad]") {
  const testing::MainThreadScope main_thread;
  const SpriteQuad quad = BuildOne(OffCentre(), 0.0F, {1.0F, 1.0F});
  // Corners TL, TR, BR, BL of the region, (2, 1) up and left of (10, 20).
  CHECK(quad.pos[0] == Vec2{8.0F, 19.0F});
  CHECK(quad.pos[1] == Vec2{16.0F, 19.0F});
  CHECK(quad.pos[2] == Vec2{16.0F, 23.0F});
  CHECK(quad.pos[3] == Vec2{8.0F, 23.0F});
  // Texel coordinates of the region, in the same corner order.
  CHECK(quad.uv[0] == Vec2{16.0F, 32.0F});
  CHECK(quad.uv[1] == Vec2{24.0F, 32.0F});
  CHECK(quad.uv[2] == Vec2{24.0F, 36.0F});
  CHECK(quad.uv[3] == Vec2{16.0F, 36.0F});
  CHECK(quad.tint == Rgba8{10, 20, 30, 40});
}

TEST_CASE("Scale and rotation pivot about the origin", "[render][quad]") {
  const testing::MainThreadScope main_thread;
  SECTION("scale") {
    const SpriteQuad quad = BuildOne(OffCentre(), 0.0F, {2.0F, 3.0F});
    CHECK(quad.pos[0] == Vec2{6.0F, 17.0F});
    CHECK(quad.pos[2] == Vec2{22.0F, 29.0F});
  }
  SECTION("90 degrees is clockwise on screen (y down)") {
    // Local corner (x, y) goes to (-y, x).
    const SpriteQuad quad = BuildOne(OffCentre(), 90.0F, {1.0F, 1.0F});
    CheckPos(quad, 0, 11.0F, 18.0F);  // (-2, -1) -> (1, -2)
    CheckPos(quad, 1, 11.0F, 26.0F);  // (6, -1) -> (1, 6)
    CheckPos(quad, 2, 7.0F, 26.0F);   // (6, 3) -> (-3, 6)
    CheckPos(quad, 3, 7.0F, 18.0F);   // (-2, 3) -> (-3, -2)
  }
  SECTION("180 degrees") {
    const SpriteQuad quad = BuildOne(OffCentre(), 180.0F, {1.0F, 1.0F});
    CheckPos(quad, 0, 12.0F, 21.0F);
    CheckPos(quad, 2, 4.0F, 17.0F);
  }
}

TEST_CASE("Flips mirror the sprite about its origin", "[render][quad]") {
  const testing::MainThreadScope main_thread;
  SpriteRenderer sprite = OffCentre();
  SECTION("flip_x") {
    sprite.flip_x = true;
    const SpriteQuad quad = BuildOne(sprite, 0.0F, {1.0F, 1.0F});
    // x spans [10 - 6, 10 + 2]; the right texel column is drawn on the left.
    CHECK(quad.pos[0] == Vec2{4.0F, 19.0F});
    CHECK(quad.pos[2] == Vec2{12.0F, 23.0F});
    CHECK(quad.uv[0] == Vec2{24.0F, 32.0F});
    CHECK(quad.uv[1] == Vec2{16.0F, 32.0F});
    CHECK(quad.uv[2] == Vec2{16.0F, 36.0F});
    CHECK(quad.uv[3] == Vec2{24.0F, 36.0F});
  }
  SECTION("flip_y") {
    sprite.flip_y = true;
    const SpriteQuad quad = BuildOne(sprite, 0.0F, {1.0F, 1.0F});
    // y spans [20 - 3, 20 + 1].
    CHECK(quad.pos[0] == Vec2{8.0F, 17.0F});
    CHECK(quad.pos[2] == Vec2{16.0F, 21.0F});
    CHECK(quad.uv[0] == Vec2{16.0F, 36.0F});
    CHECK(quad.uv[2] == Vec2{24.0F, 32.0F});
  }
  SECTION("both") {
    sprite.flip_x = true;
    sprite.flip_y = true;
    const SpriteQuad quad = BuildOne(sprite, 0.0F, {1.0F, 1.0F});
    CHECK(quad.pos[0] == Vec2{4.0F, 17.0F});
    CHECK(quad.pos[2] == Vec2{12.0F, 21.0F});
    CHECK(quad.uv[0] == Vec2{24.0F, 36.0F});
    CHECK(quad.uv[2] == Vec2{16.0F, 32.0F});
  }
  SECTION("a centred origin keeps the sprite in place") {
    SpriteRenderer centred = testing::CenteredSprite(7, 8.0F, 4.0F);
    centred.flip_x = true;
    const SpriteQuad quad = BuildOne(centred, 0.0F, {1.0F, 1.0F});
    CHECK(quad.pos[0] == Vec2{6.0F, 18.0F});
    CHECK(quad.pos[2] == Vec2{14.0F, 22.0F});
    CHECK(quad.uv[0] == Vec2{8.0F, 0.0F});
  }
  SECTION("flips rotate with the sprite") {
    sprite.flip_x = true;
    const SpriteQuad quad = BuildOne(sprite, 90.0F, {1.0F, 1.0F});
    // Flipped local corners (-6, -1) and (2, 3), turned 90 degrees.
    CheckPos(quad, 0, 11.0F, 14.0F);
    CheckPos(quad, 2, 7.0F, 22.0F);
  }
}

}  // namespace
}  // namespace se
