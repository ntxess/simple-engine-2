// Copyright 2026 The simple-engine-2 Authors.

#include "se_render_core/sprite_vertices.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "se_core/assert.h"
#include "se_core/types.h"
#include "se_render_core/render_list.h"

namespace se {

void WriteSpriteVertices(std::span<const SpriteQuad> quads,
                         std::span<SpriteVertex> out) noexcept {
  SE_ASSERT_MSG(out.size() >= quads.size() * kVerticesPerQuad,
                "WriteSpriteVertices: the vertex buffer is too small");
  SpriteVertex* vertex = out.data();
  for (const SpriteQuad& quad : quads) {
    for (std::size_t corner = 0; corner < kVerticesPerQuad; ++corner) {
      vertex->x = quad.pos[corner].x;
      vertex->y = quad.pos[corner].y;
      vertex->u = quad.uv[corner].x;
      vertex->v = quad.uv[corner].y;
      vertex->tint = quad.tint;
      ++vertex;
    }
  }
}

void WriteQuadIndices(std::span<std::uint32_t> out) noexcept {
  const std::size_t quads = out.size() / kIndicesPerQuad;
  std::uint32_t* index = out.data();
  for (std::size_t q = 0; q < quads; ++q) {
    const auto base = static_cast<std::uint32_t>(q * kVerticesPerQuad);
    index[0] = base;
    index[1] = base + 1;
    index[2] = base + 2;
    index[3] = base;
    index[4] = base + 2;
    index[5] = base + 3;
    index += kIndicesPerQuad;
  }
}

CanvasFit FitCanvas(Vec2 canvas, bool integer_scale, std::uint32_t target_width,
                    std::uint32_t target_height) noexcept {
  CanvasFit fit;
  if (!(canvas.x > 0.0F) || !(canvas.y > 0.0F) || target_width == 0 ||
      target_height == 0) {
    return fit;
  }
  const auto width = static_cast<float>(target_width);
  const auto height = static_cast<float>(target_height);
  float scale = std::min(width / canvas.x, height / canvas.y);
  if (integer_scale && scale >= 1.0F) scale = std::floor(scale);
  fit.scale = scale;
  fit.offset = {std::floor(0.5F * (width - canvas.x * scale)),
                std::floor(0.5F * (height - canvas.y * scale))};
  return fit;
}

std::array<float, 4> CanvasToClip(const CanvasFit& fit,
                                  std::uint32_t target_width,
                                  std::uint32_t target_height) noexcept {
  const float width = std::max(1.0F, static_cast<float>(target_width));
  const float height = std::max(1.0F, static_cast<float>(target_height));
  // pixel = canvas * scale + offset; clip x = 2 pixel / width - 1, clip
  // y = 1 - 2 pixel / height.
  return {2.0F * fit.scale / width, -2.0F * fit.scale / height,
          2.0F * fit.offset.x / width - 1.0F,
          1.0F - 2.0F * fit.offset.y / height};
}

}  // namespace se
