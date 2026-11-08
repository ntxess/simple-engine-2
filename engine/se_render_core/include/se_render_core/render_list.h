// Copyright 2026 The simple-engine-2 Authors.
//
// The headless RenderList builder (docs/milestone-specs.md M3c;
// docs/design/mt-design.md "Snapshot And Interpolation" and "Rendering").
// BuildRenderList reads a const World through CReg() only, so rendering
// never writes simulation state. For every entity with a SpriteRenderer, a
// Transform and a Spawned record it
//   - interpolates PrevTransform -> Transform by alpha: position and scale
//     linearly, rotation along the shorter arc. It does not interpolate
//     (draws the Transform) when Transform::teleport_tick is the World's
//     current tick, when PrevTransform is not from the tick before (older,
//     or the spawn tick itself) or when there is no PrevTransform;
//   - builds the sprite's quad in canvas pixels of the camera (rotation from
//     se::dm::SinCos, so the quads are the same on every platform);
//   - culls it unless its rotated bounds overlap the canvas with a positive
//     area (a quad that only touches an edge is culled);
//   - sorts the quads by (layer, texture, z, spawn serial). The spawn serial
//     is unique, so the order is total and never depends on EnTT's storage
//     order; -0 and +0 z compare equal;
//   - merges consecutive quads of one texture into QuadBatches.
//
// The list keeps its buffers between builds: once it has held as many
// sprites as a build needs, building allocates nothing.

#ifndef SE_RENDER_CORE_RENDER_LIST_H_
#define SE_RENDER_CORE_RENDER_LIST_H_

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"

namespace se {

// One sprite's vertices: the corners of its region in the order top-left,
// top-right, bottom-right, bottom-left of the unrotated sprite (clockwise on
// screen), each with its texture coordinate. Positions are canvas pixels of
// the camera (0..virtual_size, y down). Texture coordinates are texel
// positions inside the texture (region.x .. region.x + region.w); the
// backend divides by the texture's size, which only it knows.
struct SpriteQuad {
  std::array<Vec2, 4> pos;
  std::array<Vec2, 4> uv;
  Rgba8 tint;
};

// A run of quads [first, first + count) that share one texture.
struct QuadBatch {
  std::uint32_t texture = 0;
  std::uint32_t first = 0;
  std::uint32_t count = 0;
};

struct RenderStats {
  // Entities with a SpriteRenderer, a Transform and a Spawned record.
  std::uint32_t sprites = 0;
  // Of those, drawn between PrevTransform and Transform.
  std::uint32_t interpolated = 0;
  // Of those, outside the camera.
  std::uint32_t culled = 0;
  std::uint32_t quads = 0;
  std::uint32_t batches = 0;
  // quads * sizeof(SpriteQuad): the vertex data a backend uploads.
  std::uint64_t quad_bytes = 0;
};

// Working memory of BuildRenderList, kept so later builds reuse it.
struct RenderListScratch {
  // One entry per visible sprite: the sort key and the sprite's quad index.
  struct SortItem {
    // (z key << 32) | spawn serial.
    std::uint64_t lo = 0;
    // (layer key << 32) | texture.
    std::uint64_t hi = 0;
    std::uint32_t quad = 0;
  };

  std::vector<SpriteQuad> quads;
  std::vector<SortItem> items;
  std::vector<SortItem> items_swap;
};

struct RenderList {
  // In draw order.
  std::vector<SpriteQuad> quads;
  std::vector<QuadBatch> batches;
  RenderStats stats;
  RenderListScratch scratch;
};

// Replaces |list|'s contents with the World's sprites seen through |camera|,
// interpolated by |alpha| (clamped to [0, 1]; NaN draws the Transform). A
// camera whose zoom is not positive sees nothing.
void BuildRenderList(const World& world, const Camera2D& camera, float alpha,
                     RenderList& list);

// The primary Camera2D of the World (the one with the lowest spawn serial
// if several are marked primary), or nullopt.
[[nodiscard]] std::optional<Camera2D> PrimaryCamera(const World& world);

}  // namespace se

#endif  // SE_RENDER_CORE_RENDER_LIST_H_
