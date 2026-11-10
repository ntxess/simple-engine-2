// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor_panels/viewport_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "se_core/types.h"
#include "se_render_core/render_components.h"
#include "se_render_core/sprite_vertices.h"

namespace se::editor {

Vec2 FitAspectSize(Vec2 region, float aspect) noexcept {
  if (!(aspect > 0.0F)) return region;
  float width = region.x;
  float height = width / aspect;
  if (height > region.y) {
    height = region.y;
    width = height * aspect;
  }
  return {width, height};
}

Vec2 CenteredTopLeft(Vec2 cursor, Vec2 region, float aspect) noexcept {
  const Vec2 size = FitAspectSize(region, aspect);
  return {cursor.x + (region.x - size.x) / 2.0F,
          cursor.y + (region.y - size.y) / 2.0F};
}

Vec2 ViewportMapping::ScreenToWorld(Vec2 screen) const noexcept {
  const Vec2 target = screen - image_min;
  const Vec2 canvas = (target - fit.offset) * (1.0F / fit.scale);
  const Vec2 half = camera.virtual_size * 0.5F;
  return camera.center + (canvas - half) * (1.0F / camera.zoom);
}

Vec2 ViewportMapping::WorldToScreen(Vec2 world) const noexcept {
  const Vec2 half = camera.virtual_size * 0.5F;
  const Vec2 canvas = (world - camera.center) * camera.zoom + half;
  return image_min + fit.offset + canvas * fit.scale;
}

float ViewportMapping::Scale() const noexcept {
  return fit.scale * camera.zoom;
}

bool ViewportMapping::Contains(Vec2 screen) const noexcept {
  const Vec2 local = screen - image_min;
  return local.x >= 0.0F && local.y >= 0.0F &&
         local.x < static_cast<float>(target_width) &&
         local.y < static_cast<float>(target_height);
}

namespace {

std::uint32_t WholePixels(float extent) noexcept {
  if (!(extent >= 1.0F)) return 1;
  return static_cast<std::uint32_t>(std::min(extent, 16384.0F));
}

}  // namespace

ViewportMapping MapViewport(Vec2 cursor, Vec2 region,
                            const Camera2D& camera) noexcept {
  const float aspect = camera.virtual_size.y > 0.0F
                           ? camera.virtual_size.x / camera.virtual_size.y
                           : 0.0F;
  const Vec2 size = FitAspectSize(region, aspect);
  const Vec2 top_left = CenteredTopLeft(cursor, region, aspect);
  ViewportMapping mapping;
  mapping.image_min = {std::floor(top_left.x), std::floor(top_left.y)};
  mapping.target_width = WholePixels(size.x);
  mapping.target_height = WholePixels(size.y);
  mapping.camera = camera;
  mapping.fit = FitCanvas(camera.virtual_size, camera.integer_scale,
                          mapping.target_width, mapping.target_height);
  return mapping;
}

Camera2D PanCamera(const Camera2D& camera, Vec2 screen_delta,
                   float scale) noexcept {
  Camera2D panned = camera;
  if (scale > 0.0F) {
    panned.center = camera.center - screen_delta * (1.0F / scale);
  }
  return panned;
}

Camera2D ZoomCamera(const ViewportMapping& mapping, Vec2 screen,
                    float wheel) noexcept {
  Camera2D zoomed = mapping.camera;
  if (wheel == 0.0F) return zoomed;
  const Vec2 anchor = mapping.ScreenToWorld(screen);
  zoomed.zoom =
      std::clamp(mapping.camera.zoom * std::pow(kViewportZoomPerNotch, wheel),
                 kViewportZoomMin, kViewportZoomMax);
  // Keep |anchor| under the cursor: same canvas position, new zoom.
  const Vec2 half = mapping.camera.virtual_size * 0.5F;
  const Vec2 canvas = (screen - mapping.image_min - mapping.fit.offset) *
                      (1.0F / mapping.fit.scale);
  zoomed.center = anchor - (canvas - half) * (1.0F / zoomed.zoom);
  return zoomed;
}

}  // namespace se::editor
