// Copyright 2026 The simple-engine-2 Authors.
//
// The Viewport panel's geometry: the legacy letterbox math (scaleSize and
// getCenteredTLPos, app/editor/Editor.cpp:632-656 at master), the mapping
// between screen and world pixels through FitCanvas, and the editor
// camera's pan and zoom.

#include "se_editor_panels/viewport_math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "se_core/types.h"
#include "se_render_core/render_components.h"

namespace se::editor {
namespace {

using Catch::Approx;

Camera2D ArenaCamera() {
  Camera2D camera;
  camera.center = {960.0F, 540.0F};
  camera.virtual_size = {1920.0F, 1080.0F};
  return camera;
}

TEST_CASE("The letterbox fits the aspect width first, then height",
          "[editor][viewport]") {
  const float aspect = 16.0F / 9.0F;
  // A wide region: the height decides, centred horizontally.
  REQUIRE(FitAspectSize({1000.0F, 360.0F}, aspect) == Vec2{640.0F, 360.0F});
  REQUIRE(CenteredTopLeft({10.0F, 20.0F}, {1000.0F, 360.0F}, aspect) ==
          Vec2{190.0F, 20.0F});
  // A tall region: the width decides, centred vertically.
  REQUIRE(FitAspectSize({640.0F, 1000.0F}, aspect) == Vec2{640.0F, 360.0F});
  REQUIRE(CenteredTopLeft({0.0F, 0.0F}, {640.0F, 1000.0F}, aspect) ==
          Vec2{0.0F, 320.0F});
  // An exact fit and a bad aspect.
  REQUIRE(FitAspectSize({1920.0F, 1080.0F}, aspect) == Vec2{1920.0F, 1080.0F});
  REQUIRE(FitAspectSize({300.0F, 200.0F}, 0.0F) == Vec2{300.0F, 200.0F});
}

TEST_CASE("Screen and world pixels map both ways through the target",
          "[editor][viewport]") {
  const ViewportMapping mapping =
      MapViewport({100.0F, 50.0F}, {960.0F, 700.0F}, ArenaCamera());
  REQUIRE(mapping.target_width == 960);
  REQUIRE(mapping.target_height == 540);
  REQUIRE(mapping.image_min == Vec2{100.0F, 130.0F});
  REQUIRE(mapping.fit.scale == Approx(0.5F));
  REQUIRE(mapping.Scale() == Approx(0.5F));
  // The image's corners are the arena's.
  REQUIRE(mapping.ScreenToWorld({100.0F, 130.0F}) == Vec2{0.0F, 0.0F});
  REQUIRE(mapping.WorldToScreen({1920.0F, 1080.0F}) == Vec2{1060.0F, 670.0F});
  const Vec2 world{333.0F, 777.0F};
  const Vec2 back = mapping.ScreenToWorld(mapping.WorldToScreen(world));
  REQUIRE(back.x == Approx(world.x));
  REQUIRE(back.y == Approx(world.y));
  REQUIRE(mapping.Contains({100.0F, 130.0F}));
  REQUIRE_FALSE(mapping.Contains({99.0F, 130.0F}));
  REQUIRE_FALSE(mapping.Contains({1060.0F, 300.0F}));
}

TEST_CASE("A tiny region still gets a 1x1 target", "[editor][viewport]") {
  const ViewportMapping mapping = MapViewport({}, {0.0F, 0.0F}, ArenaCamera());
  REQUIRE(mapping.target_width == 1);
  REQUIRE(mapping.target_height == 1);
}

TEST_CASE("A pan moves the camera by the drag in world pixels",
          "[editor][viewport]") {
  const Camera2D camera = ArenaCamera();
  const Camera2D panned = PanCamera(camera, {50.0F, -20.0F}, 0.5F);
  REQUIRE(panned.center == Vec2{860.0F, 580.0F});
  REQUIRE(panned.zoom == camera.zoom);
  REQUIRE(PanCamera(camera, {50.0F, 0.0F}, 0.0F).center == camera.center);
}

TEST_CASE("The wheel zooms about the cursor within the limits",
          "[editor][viewport]") {
  const ViewportMapping mapping =
      MapViewport({0.0F, 0.0F}, {960.0F, 540.0F}, ArenaCamera());
  const Vec2 cursor{240.0F, 135.0F};
  const Vec2 anchor = mapping.ScreenToWorld(cursor);
  const Camera2D zoomed = ZoomCamera(mapping, cursor, 2.0F);
  REQUIRE(zoomed.zoom == Approx(1.21F));
  ViewportMapping after = mapping;
  after.camera = zoomed;
  const Vec2 still = after.ScreenToWorld(cursor);
  REQUIRE(still.x == Approx(anchor.x));
  REQUIRE(still.y == Approx(anchor.y));
  REQUIRE(ZoomCamera(mapping, cursor, 1000.0F).zoom == kViewportZoomMax);
  REQUIRE(ZoomCamera(mapping, cursor, -1000.0F).zoom == kViewportZoomMin);
  REQUIRE(ZoomCamera(mapping, cursor, 0.0F).center == mapping.camera.center);
}

}  // namespace
}  // namespace se::editor
