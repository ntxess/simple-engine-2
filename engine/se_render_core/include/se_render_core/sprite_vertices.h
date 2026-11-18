// Copyright 2026 The simple-engine-2 Authors.
//
// The CPU side of the sprite pipeline (docs/milestone-specs.md M7): the
// vertex format Renderer2D uploads, vertex and index generation from a
// RenderList's quads, and the camera transform from a camera's canvas to a
// render target. Headless, so se_bench measures it (bench.render_frame_10k)
// and the WSL sanitizer builds test it.
//
// Vertices stay in canvas pixels with texel coordinates, as the RenderList
// gives them (ADR-022): the vertex shader maps canvas pixels to clip space
// with CanvasToClip and texels to [0, 1] with the texture's size, per draw,
// so a texture reloaded at another size needs no new vertices.

#ifndef SE_RENDER_CORE_SPRITE_VERTICES_H_
#define SE_RENDER_CORE_SPRITE_VERTICES_H_

#include <array>
#include <cstdint>
#include <span>

#include "se_core/types.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"

namespace se {

// One corner of a sprite quad: canvas pixels, texel coordinates and the
// tint (unsigned normalised bytes in the shader). 20 bytes.
struct SpriteVertex {
  float x = 0.0F;
  float y = 0.0F;
  float u = 0.0F;
  float v = 0.0F;
  Rgba8 tint;
};
static_assert(sizeof(SpriteVertex) == 20);

inline constexpr std::uint32_t kVerticesPerQuad = 4;
inline constexpr std::uint32_t kIndicesPerQuad = 6;

// Writes the four corners of every quad, in the RenderList's corner order
// (top-left, top-right, bottom-right, bottom-left), into |out|, which holds
// at least quads.size() * kVerticesPerQuad vertices. Allocates nothing.
void WriteSpriteVertices(std::span<const SpriteQuad> quads,
                         std::span<SpriteVertex> out) noexcept;

// Writes the indices of quads [0, out.size() / 6): two triangles per quad,
// (0, 1, 2) and (0, 2, 3) of its corners. The pattern only depends on the
// quad's position, so one index buffer serves every frame.
void WriteQuadIndices(std::span<std::uint32_t> out) noexcept;

// Where a camera's canvas lands in a render target: canvas pixel p is drawn
// at target pixel p * scale + offset. The canvas keeps its aspect ratio
// (letterboxed), is centred, and with Camera2D::integer_scale is scaled by
// a whole multiple when it fits at least once. Offsets are whole pixels.
struct CanvasFit {
  float scale = 1.0F;
  Vec2 offset;
};

[[nodiscard]] CanvasFit FitCanvas(Vec2 canvas, bool integer_scale,
                                  std::uint32_t target_width,
                                  std::uint32_t target_height) noexcept;

// The vertex shader's canvas-to-clip transform for |fit| in a target of
// the given size: clip = canvas * (x, y) + (z, w), with y up in clip space
// and down in pixels.
[[nodiscard]] std::array<float, 4> CanvasToClip(
    const CanvasFit& fit, std::uint32_t target_width,
    std::uint32_t target_height) noexcept;

}  // namespace se

#endif  // SE_RENDER_CORE_SPRITE_VERTICES_H_
