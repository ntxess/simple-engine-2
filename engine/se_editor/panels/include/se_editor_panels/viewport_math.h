// Copyright 2026 The simple-engine-2 Authors.
//
// The Viewport panel's geometry: the letterbox math ported from the legacy
// scene view (app/editor/Editor.cpp:451-464 and :632-656 at master), the
// editor camera's pan and zoom, and the mapping between screen pixels and
// world pixels through a ViewportTarget.
//
// Layout. FitAspectSize is the legacy scaleSize: the largest size with the
// camera's aspect that fits the panel's content region; CenteredTopLeft is
// getCenteredTLPos: that size centred in the region. The panel requests its
// ViewportTarget at that size in whole pixels and draws it 1:1 there, so a
// target texel is a screen pixel. The renderer then fits the camera's canvas
// into the target with FitCanvas (se_render_core/sprite_vertices.h), which
// MapViewport uses too: screen -> target pixel -> canvas pixel -> world.
//
// Pan and zoom (the editor camera, never the World's Camera2D): a right or
// middle drag moves the camera by the drag in world pixels; the wheel
// multiplies the zoom by 1.1 per notch within [kViewportZoomMin,
// kViewportZoomMax], keeping the world point under the cursor in place.

#ifndef SE_EDITOR_PANELS_VIEWPORT_MATH_H_
#define SE_EDITOR_PANELS_VIEWPORT_MATH_H_

#include <cstdint>

#include "se_core/types.h"
#include "se_render_core/render_components.h"
#include "se_render_core/sprite_vertices.h"

namespace se::editor {

inline constexpr float kViewportZoomMin = 0.05F;
inline constexpr float kViewportZoomMax = 40.0F;
inline constexpr float kViewportZoomPerNotch = 1.1F;

// The legacy scaleSize (Editor.cpp:632-642): width first, then height if
// the width-derived height does not fit. A non-positive aspect gives
// |region|.
[[nodiscard]] Vec2 FitAspectSize(Vec2 region, float aspect) noexcept;

// The legacy getCenteredTLPos (Editor.cpp:644-656): the fitted size centred
// in the region whose top-left corner is |cursor|.
[[nodiscard]] Vec2 CenteredTopLeft(Vec2 cursor, Vec2 region,
                                   float aspect) noexcept;

// Where the panel draws its target, and how screen pixels map to the World.
struct ViewportMapping {
  // The image's top-left corner on screen and its size in pixels (whole
  // pixels, the target's size).
  Vec2 image_min;
  std::uint32_t target_width = 0;
  std::uint32_t target_height = 0;
  Camera2D camera;
  CanvasFit fit;

  [[nodiscard]] Vec2 ScreenToWorld(Vec2 screen) const noexcept;
  [[nodiscard]] Vec2 WorldToScreen(Vec2 world) const noexcept;
  // Screen pixels per world pixel.
  [[nodiscard]] float Scale() const noexcept;
  // Whether |screen| lies on the image.
  [[nodiscard]] bool Contains(Vec2 screen) const noexcept;
};

// Lays the target out in the content region at |cursor| of size |region|
// (the camera's virtual size gives the aspect), at least 1x1 pixels.
[[nodiscard]] ViewportMapping MapViewport(Vec2 cursor, Vec2 region,
                                          const Camera2D& camera) noexcept;

// The editor camera after a pan by |screen_delta| pixels at |scale| screen
// pixels per world pixel.
[[nodiscard]] Camera2D PanCamera(const Camera2D& camera, Vec2 screen_delta,
                                 float scale) noexcept;

// The editor camera after |wheel| notches with the cursor at |screen|,
// keeping the world point under it fixed.
[[nodiscard]] Camera2D ZoomCamera(const ViewportMapping& mapping, Vec2 screen,
                                  float wheel) noexcept;

}  // namespace se::editor

#endif  // SE_EDITOR_PANELS_VIEWPORT_MATH_H_
