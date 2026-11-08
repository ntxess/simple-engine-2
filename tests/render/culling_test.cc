// Copyright 2026 The simple-engine-2 Authors.
//
// BuildRenderList culling: rotated bounds against the camera's canvas, with
// a positive-area overlap required, and the camera's centre and zoom.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <cstdint>

#include "render/render_fixture.h"
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

// The number of quads one sprite gives through |camera|.
std::size_t Visible(Vec2 pos, const SpriteRenderer& sprite, float rot,
                    const Camera2D& camera) SE_REQUIRES(g_main) {
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  static_cast<void>(testing::AddSprite(world, 1, pos, sprite, rot));
  RenderList list;
  BuildRenderList(world, camera, 1.0F, list);
  REQUIRE(list.stats.sprites == 1);
  REQUIRE(list.stats.culled + list.stats.quads == 1);
  return list.quads.size();
}

TEST_CASE("Sprites that only touch a camera edge are culled",
          "[render][culling]") {
  const testing::MainThreadScope main_thread;
  const Camera2D camera = testing::WorldCamera(100.0F, 80.0F);
  const SpriteRenderer sprite = testing::CenteredSprite(1, 10.0F, 10.0F);
  // Left and right: the sprite spans [x - 5, x + 5].
  REQUIRE(Visible({-5.0F, 40.0F}, sprite, 0.0F, camera) == 0);
  REQUIRE(Visible({-4.5F, 40.0F}, sprite, 0.0F, camera) == 1);
  REQUIRE(Visible({105.0F, 40.0F}, sprite, 0.0F, camera) == 0);
  REQUIRE(Visible({104.5F, 40.0F}, sprite, 0.0F, camera) == 1);
  // Top and bottom.
  REQUIRE(Visible({50.0F, -5.0F}, sprite, 0.0F, camera) == 0);
  REQUIRE(Visible({50.0F, -4.5F}, sprite, 0.0F, camera) == 1);
  REQUIRE(Visible({50.0F, 85.0F}, sprite, 0.0F, camera) == 0);
  REQUIRE(Visible({50.0F, 84.5F}, sprite, 0.0F, camera) == 1);
  // Corners.
  REQUIRE(Visible({-5.0F, -5.0F}, sprite, 0.0F, camera) == 0);
  REQUIRE(Visible({-4.5F, -4.5F}, sprite, 0.0F, camera) == 1);
  REQUIRE(Visible({105.0F, 85.0F}, sprite, 0.0F, camera) == 0);
  REQUIRE(Visible({104.5F, 84.5F}, sprite, 0.0F, camera) == 1);
  // Inside, and far outside.
  REQUIRE(Visible({50.0F, 40.0F}, sprite, 0.0F, camera) == 1);
  REQUIRE(Visible({-1e6F, 40.0F}, sprite, 0.0F, camera) == 0);
}

TEST_CASE("Culling uses the rotated bounds", "[render][culling]") {
  const testing::MainThreadScope main_thread;
  const Camera2D camera = testing::WorldCamera(100.0F, 80.0F);
  // 20 x 2: unrotated it spans x +-10, y +-1; at 90 degrees x +-1, y +-10.
  const SpriteRenderer bar = testing::CenteredSprite(1, 20.0F, 2.0F);
  REQUIRE(Visible({-3.0F, 40.0F}, bar, 0.0F, camera) == 1);
  REQUIRE(Visible({-3.0F, 40.0F}, bar, 90.0F, camera) == 0);
  REQUIRE(Visible({50.0F, -5.0F}, bar, 0.0F, camera) == 0);
  REQUIRE(Visible({50.0F, -5.0F}, bar, 90.0F, camera) == 1);
  // At 45 degrees the half extent is (10 + 1) / sqrt(2) = 7.78 per axis.
  REQUIRE(Visible({-7.5F, 40.0F}, bar, 45.0F, camera) == 1);
  REQUIRE(Visible({-8.0F, 40.0F}, bar, 45.0F, camera) == 0);
}

TEST_CASE("The camera's centre and zoom map the world onto the canvas",
          "[render][culling]") {
  const testing::MainThreadScope main_thread;
  Camera2D camera;
  camera.center = {50.0F, 50.0F};
  camera.zoom = 2.0F;
  camera.virtual_size = {100.0F, 100.0F};
  // The canvas shows the world rectangle [25, 75] x [25, 75].
  const SpriteRenderer sprite = testing::CenteredSprite(1, 10.0F, 10.0F);
  REQUIRE(Visible({20.0F, 50.0F}, sprite, 0.0F, camera) == 0);
  REQUIRE(Visible({21.0F, 50.0F}, sprite, 0.0F, camera) == 1);
  REQUIRE(Visible({80.0F, 50.0F}, sprite, 0.0F, camera) == 0);
  REQUIRE(Visible({79.0F, 50.0F}, sprite, 0.0F, camera) == 1);

  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  static_cast<void>(testing::AddSprite(world, 1, {60.0F, 45.0F}, sprite));
  RenderList list;
  BuildRenderList(world, camera, 1.0F, list);
  REQUIRE(list.quads.size() == 1);
  // (pos - centre) * zoom + canvas / 2, with the size scaled by the zoom.
  CHECK_THAT(list.quads[0].pos[0].x, WithinAbs(60.0F, 1e-4));
  CHECK_THAT(list.quads[0].pos[0].y, WithinAbs(30.0F, 1e-4));
  CHECK_THAT(list.quads[0].pos[2].x, WithinAbs(80.0F, 1e-4));
  CHECK_THAT(list.quads[0].pos[2].y, WithinAbs(50.0F, 1e-4));
}

TEST_CASE("A camera without a positive zoom sees nothing",
          "[render][culling]") {
  const testing::MainThreadScope main_thread;
  const SpriteRenderer sprite = testing::CenteredSprite(1, 10.0F, 10.0F);
  Camera2D camera = testing::WorldCamera(100.0F, 100.0F);
  for (const float zoom : {0.0F, -1.0F}) {
    camera.zoom = zoom;
    const InlineOwnerScope owner(OnMainThread());
    World world(testing::RenderRegistry(), {});
    static_cast<void>(testing::AddSprite(world, 1, {50.0F, 50.0F}, sprite));
    RenderList list;
    BuildRenderList(world, camera, 1.0F, list);
    CHECK(list.quads.empty());
    CHECK(list.batches.empty());
    REQUIRE(list.stats.quads == 0);
  }
}

TEST_CASE("Stats count sprites, culled sprites, quads and batches",
          "[render][culling]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  const Camera2D camera = testing::WorldCamera(100.0F, 100.0F);
  for (std::uint64_t i = 0; i < 10; ++i) {
    // Every third sprite is off screen; textures alternate in pairs.
    const float x = i % 3 == 0 ? -50.0F : 10.0F * static_cast<float>(i);
    static_cast<void>(
        testing::AddSprite(world, i + 1, {x, 50.0F},
                           testing::CenteredSprite(
                               static_cast<std::uint32_t>(i / 2), 4.0F, 4.0F)));
  }
  RenderList list;
  BuildRenderList(world, camera, 1.0F, list);
  REQUIRE(list.stats.sprites == 10);
  REQUIRE(list.stats.culled == 4);
  REQUIRE(list.stats.quads == 6);
  REQUIRE(list.quads.size() == 6);
  // Visible i = 1, 2, 4, 5, 7, 8: textures 0, 1, 2, 2, 3, 4.
  REQUIRE(list.stats.batches == 5);
  CHECK(list.stats.quad_bytes == 6 * sizeof(SpriteQuad));
}

}  // namespace
}  // namespace se
