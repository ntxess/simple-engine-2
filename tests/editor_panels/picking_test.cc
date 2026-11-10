// Copyright 2026 The simple-engine-2 Authors.
//
// Viewport picking: sprite bounds as the renderer draws them, the topmost
// sprite in draw order first, and the three paths (the broadphase scratch
// at a recorded tick, exact bounds otherwise, brute force in a faulted
// World) giving the same answers.

#include "se_editor_panels/picking.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <limits>
#include <vector>

#include "editor_panels/support/panel_world.h"
#include "se_core/core_components.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

using Catch::Approx;

std::vector<entt::entity> Entities(const PickResult& result) {
  std::vector<entt::entity> out;
  out.reserve(result.hits.size());
  for (const PickHit& hit : result.hits) out.push_back(hit.entity);
  return out;
}

TEST_CASE("Sprite bounds follow the origin, scale, flips and rotation",
          "[editor][picking]") {
  Transform transform;
  transform.pos = {100.0F, 50.0F};
  SpriteRenderer sprite;
  sprite.region = {0.0F, 0.0F, 40.0F, 20.0F};
  sprite.origin = {10.0F, 5.0F};
  PickBounds bounds = SpriteBounds(transform, sprite);
  REQUIRE(bounds.min == Vec2{90.0F, 45.0F});
  REQUIRE(bounds.max == Vec2{130.0F, 65.0F});

  transform.scale = {2.0F, 0.5F};
  bounds = SpriteBounds(transform, sprite);
  REQUIRE(bounds.min == Vec2{80.0F, 47.5F});
  REQUIRE(bounds.max == Vec2{160.0F, 57.5F});

  // A horizontal flip mirrors the rectangle about the origin.
  transform.scale = {1.0F, 1.0F};
  sprite.flip_x = true;
  bounds = SpriteBounds(transform, sprite);
  REQUIRE(bounds.min == Vec2{70.0F, 45.0F});
  REQUIRE(bounds.max == Vec2{110.0F, 65.0F});

  // A quarter turn (clockwise on screen) swaps the extents.
  sprite.flip_x = false;
  sprite.origin = {20.0F, 10.0F};
  transform.rot = 1.5707963F;
  bounds = SpriteBounds(transform, sprite);
  REQUIRE(bounds.min.x == Approx(90.0F).margin(1e-3));
  REQUIRE(bounds.max.x == Approx(110.0F).margin(1e-3));
  REQUIRE(bounds.min.y == Approx(30.0F).margin(1e-3));
  REQUIRE(bounds.max.y == Approx(70.0F).margin(1e-3));
}

TEST_CASE("A point hits a box with min <= p < max", "[editor][picking]") {
  const PickBounds box{{0.0F, 0.0F}, {10.0F, 10.0F}};
  REQUIRE(BoundsContain(box, {0.0F, 0.0F}));
  REQUIRE(BoundsContain(box, {9.99F, 5.0F}));
  REQUIRE_FALSE(BoundsContain(box, {10.0F, 5.0F}));
  REQUIRE_FALSE(BoundsContain(box, {5.0F, -0.01F}));
  const PickBounds nan{{0.0F, 0.0F},
                       {std::numeric_limits<float>::quiet_NaN(), 10.0F}};
  REQUIRE_FALSE(BoundsContain(nan, {5.0F, 5.0F}));
}

TEST_CASE("The topmost sprite comes first: layer, texture, z, spawn order",
          "[editor][picking]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::PanelWorld w;
  const entt::entity back = w.AddSprite({100.0F, 100.0F}, 48, 48, 0, 0.0F);
  const entt::entity front = w.AddSprite({110.0F, 100.0F}, 48, 48, 2, -5.0F);
  const entt::entity high_z = w.AddSprite({90.0F, 100.0F}, 48, 48, 0, 3.0F);
  const entt::entity later = w.AddSprite({100.0F, 95.0F}, 48, 48, 0, 0.0F);
  const entt::entity far = w.AddSprite({400.0F, 400.0F});
  w.Finish();

  const PickResult result = Pick(w.CGet(), {100.0F, 100.0F}, false, nullptr);
  REQUIRE(result.path == PickPath::kExact);
  REQUIRE(Entities(result) ==
          std::vector<entt::entity>{front, high_z, later, back});
  REQUIRE(result.hits.front().guid == w.GuidOf(front));
  REQUIRE(
      Pick(w.CGet(), {400.0F, 400.0F}, false, nullptr).hits.front().entity ==
      far);
  REQUIRE(Pick(w.CGet(), {700.0F, 10.0F}, false, nullptr).Empty());
}

TEST_CASE("Edit documents pick by exact bounds, never the scratch",
          "[editor][picking]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::PanelWorld w(WorldKind::kEditDocument);
  w.AddSprite({100.0F, 100.0F});
  w.Finish();
  PickScratch scratch;
  scratch.Refresh(w.CGet());
  REQUIRE_FALSE(scratch.ValidFor(w.CGet()));
  REQUIRE(scratch.Builds() == 0);
  REQUIRE(Pick(w.CGet(), {100.0F, 100.0F}, false, &scratch).path ==
          PickPath::kExact);
}

TEST_CASE("A Play World at its recorded tick picks through the scratch",
          "[editor][picking]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::PanelWorld w(WorldKind::kPlay);
  const entt::entity a = w.AddSprite({100.0F, 100.0F}, 48, 48, 0, 1.0F);
  const entt::entity b = w.AddSprite({120.0F, 100.0F}, 48, 48, 0, 2.0F);
  for (int i = 0; i < 300; ++i) {
    w.AddSprite({static_cast<float>(i * 7 % 1900), 600.0F});
  }
  w.Finish();
  w.Step();

  PickScratch scratch;
  // Without a refresh the scratch is not valid yet.
  REQUIRE(Pick(w.CGet(), {110.0F, 100.0F}, false, &scratch).path ==
          PickPath::kExact);
  scratch.Refresh(w.CGet());
  REQUIRE(scratch.ValidFor(w.CGet()));
  const PickResult fast = Pick(w.CGet(), {110.0F, 100.0F}, false, &scratch);
  REQUIRE(fast.path == PickPath::kBroadphase);
  REQUIRE(Entities(fast) == std::vector<entt::entity>{b, a});
  // Same answers as the exact path everywhere.
  for (float x = 0.0F; x < 1920.0F; x += 37.0F) {
    for (float y = 0.0F; y < 700.0F; y += 23.0F) {
      CHECK(Entities(Pick(w.CGet(), {x, y}, false, &scratch)) ==
            Entities(Pick(w.CGet(), {x, y}, false, nullptr)));
    }
  }
  // A second refresh at the same tick rebuilds nothing.
  scratch.Refresh(w.CGet());
  REQUIRE(scratch.Builds() == 1);

  // A paused edit marks the World changed: exact bounds until the next
  // tick records a hash again.
  {
    const MutationGate gate = w.Get().OpenGate(GateReason::kTest);
    w.Get().Reg().get<Transform>(a).pos = {500.0F, 500.0F};
  }
  REQUIRE_FALSE(scratch.ValidFor(w.CGet()));
  REQUIRE(Pick(w.CGet(), {500.0F, 500.0F}, false, &scratch).path ==
          PickPath::kExact);
  REQUIRE(Entities(Pick(w.CGet(), {500.0F, 500.0F}, false, &scratch)) ==
          std::vector<entt::entity>{a});
  w.Step();
  scratch.Refresh(w.CGet());
  REQUIRE(scratch.Builds() == 2);
  REQUIRE(Entities(Pick(w.CGet(), {500.0F, 500.0F}, false, &scratch)) ==
          std::vector<entt::entity>{a});
}

TEST_CASE("A faulted World picks by brute force over the id storage",
          "[editor][picking]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::PanelWorld w(WorldKind::kPlay);
  const entt::entity a = w.AddSprite({100.0F, 100.0F}, 48, 48, 0, 1.0F);
  const entt::entity b = w.AddSprite({120.0F, 100.0F}, 48, 48, 0, 2.0F);
  w.Finish();
  w.Step();
  // A throw can leave an entity without its Spawned record or Transform.
  const entt::entity c = w.AddSprite({100.0F, 100.0F});
  {
    const MutationGate gate = w.Get().OpenGate(GateReason::kTest);
    w.Get().Reg().remove<Spawned>(b);
    w.Get().Reg().remove<Transform>(c);
  }

  PickScratch scratch;
  scratch.Refresh(w.CGet());
  const PickResult result = Pick(w.CGet(), {110.0F, 100.0F}, true, &scratch);
  REQUIRE(result.path == PickPath::kBruteForce);
  REQUIRE(Entities(result) == std::vector<entt::entity>{b, a});
  // The exact path needs a Spawned record (what the renderer draws).
  REQUIRE(Entities(Pick(w.CGet(), {110.0F, 100.0F}, false, nullptr)) ==
          std::vector<entt::entity>{a});
}

}  // namespace
}  // namespace se::editor
