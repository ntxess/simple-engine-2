// Copyright 2026 The simple-engine-2 Authors.
//
// The renderer's components (docs/milestone-specs.md, M3c). Both are
// serialized but not hashed: what an entity looks like never feeds the
// simulation, so changing a sprite does not change the world hash.
//
// Units: positions and sizes are world pixels, y grows downwards, and
// Transform::rot is in radians, clockwise on screen (ADR-023).

#ifndef SE_RENDER_CORE_RENDER_COMPONENTS_H_
#define SE_RENDER_CORE_RENDER_COMPONENTS_H_

#include <cstdint>
#include <string_view>

#include "se_core/component_registry.h"
#include "se_core/hash.h"
#include "se_core/types.h"

namespace se {

// The interned key of the texture named |name| (a key of
// config/texture.json): its 32-bit FNV-1a hash, until M9's asset handles.
// Scenes write the name; SpriteRenderer stores the key.
[[nodiscard]] constexpr std::uint32_t TextureKeyOf(
    std::string_view name) noexcept {
  return Fnv1a32(name);
}

// A textured quad drawn at the entity's Transform.
struct SpriteRenderer {
  // The texture: an interned asset key (TextureKeyOf) until M9's asset
  // handles.
  std::uint32_t texture = 0;
  // The texels to draw, in texture pixels. Its size is the sprite's size
  // before Transform::scale.
  Rect region;
  // The pivot, in pixels from the region's top-left corner: the point drawn
  // at Transform::pos, and the centre of rotation, scaling and flips.
  Vec2 origin;
  Rgba8 tint;
  // Draw order: layer first, then texture (for batching), then z, then the
  // spawn serial (BuildRenderList).
  std::int16_t layer = 0;
  float z = 0.0F;
  // Mirror the sprite about its origin, horizontally and vertically. Only
  // the drawing changes; colliders and the Transform do not.
  bool flip_x = false;
  bool flip_y = false;
};

// A view onto the World: the world rectangle centred on |center| that is
// virtual_size / zoom pixels large is drawn onto a canvas of virtual_size
// pixels.
struct Camera2D {
  Vec2 center;
  // Canvas pixels per world pixel; must be positive.
  float zoom = 1.0F;
  // The canvas size in pixels (the game's virtual resolution).
  Vec2 virtual_size{1920.0F, 1080.0F};
  // Scale the canvas to the window by whole multiples only (letterboxing
  // the rest). The backend applies it; the RenderList is in canvas pixels.
  bool integer_scale = false;
  // The camera the game renders to the swapchain (PrimaryCamera()).
  bool primary = false;
};

// Registers SpriteRenderer and Camera2D (render_components.cc).
void RegisterRenderComponents(ComponentRegistry& registry);

}  // namespace se

#endif  // SE_RENDER_CORE_RENDER_COMPONENTS_H_
