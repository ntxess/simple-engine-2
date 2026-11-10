// Copyright 2026 The simple-engine-2 Authors.
//
// The ported entity overlays (app/editor/Editor.cpp:538-620 at master) as
// screen-space primitives: ids, boxes red where they overlap the player's,
// headings and positions.

#include "se_editor_panels/overlays.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <entt/entity/registry.hpp>
#include <string>

#include "editor_panels/support/panel_world.h"
#include "game_rampage/components.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_editor_panels/viewport_math.h"
#include "se_render_core/render_components.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

using Catch::Approx;

ViewportMapping OneToOne() {
  Camera2D camera;
  camera.center = {960.0F, 540.0F};
  camera.virtual_size = {1920.0F, 1080.0F};
  return MapViewport({}, {1920.0F, 1080.0F}, camera);
}

TEST_CASE("No option, no overlay", "[editor][overlays]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::PanelWorld w;
  w.AddSprite({100.0F, 100.0F});
  Overlays out;
  out.texts.push_back({});
  BuildOverlays(w.CGet(), OverlayOptions{}, OneToOne(), out);
  REQUIRE(out.quads.empty());
  REQUIRE(out.lines.empty());
  REQUIRE(out.texts.empty());
}

TEST_CASE("Boxes are colliders, red where they overlap the player's",
          "[editor][overlays]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::PanelWorld w;
  const entt::entity player = w.AddSprite({100.0F, 100.0F});
  const entt::entity touching = w.AddSprite({140.0F, 100.0F});
  const entt::entity apart = w.AddSprite({400.0F, 100.0F});
  entt::registry& reg = w.Get().Reg();
  reg.emplace<rampage::PlayerControl>(player);
  reg.emplace<rampage::Collider>(player, Vec2{24.0F, 24.0F});
  reg.emplace<rampage::Collider>(touching, Vec2{24.0F, 24.0F});
  static_cast<void>(apart);

  OverlayOptions options;
  options.boxes = true;
  Overlays out;
  BuildOverlays(w.CGet(), options, OneToOne(), out);
  REQUIRE(out.quads.size() == 3);
  // In view order; find each by its first corner.
  int red = 0;
  int green = 0;
  for (const OverlayQuad& quad : out.quads) {
    if (quad.corners[0] == Vec2{116.0F, 76.0F}) {
      REQUIRE(quad.color == kOverlayRed);
      REQUIRE(quad.corners[2] == Vec2{164.0F, 124.0F});
    }
    if (quad.corners[0] == Vec2{76.0F, 76.0F}) {
      // The player never collides with itself.
      REQUIRE(quad.color == kOverlayGreen);
    }
    if (quad.corners[0] == Vec2{376.0F, 76.0F}) {
      // Without a collider: the sprite's bounds.
      REQUIRE(quad.color == kOverlayGreen);
    }
    red += quad.color == kOverlayRed ? 1 : 0;
    green += quad.color == kOverlayGreen ? 1 : 0;
  }
  REQUIRE(red == 1);
  REQUIRE(green == 2);
}

TEST_CASE("Headings, ids and positions sit where the legacy drew them",
          "[editor][overlays]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::PanelWorld w;
  const entt::entity e = w.AddSprite({200.0F, 300.0F}, 40.0F, 20.0F);
  OverlayOptions options;
  options.headings = true;
  options.ids = true;
  options.positions = true;
  Overlays out;
  BuildOverlays(w.CGet(), options, OneToOne(), out);
  REQUIRE(out.lines.size() == 2);
  // East along the rotation (0), north a quarter turn before it.
  REQUIRE(out.lines[0].from == Vec2{200.0F, 300.0F});
  REQUIRE(out.lines[0].to.x == Approx(250.0F));
  REQUIRE(out.lines[0].to.y == Approx(300.0F));
  REQUIRE(out.lines[1].to.x == Approx(200.0F).margin(1e-3));
  REQUIRE(out.lines[1].to.y == Approx(250.0F));
  REQUIRE(out.texts.size() == 2);
  REQUIRE(out.texts[0].text == ToString(w.GuidOf(e)).substr(0, 8));
  REQUIRE(out.texts[0].pos == Vec2{220.0F, 310.0F});
  REQUIRE(out.texts[1].text == "200.00, 300.00");
  REQUIRE(out.texts[1].pos == Vec2{220.0F, 300.0F});
}

}  // namespace
}  // namespace se::editor
