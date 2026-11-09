// Copyright 2026 The simple-engine-2 Authors.
//
// The CPU side of the sprite pipeline (se_render_core/sprite_vertices.h):
// vertices from RenderList quads, the shared quad index pattern, and the
// canvas-to-target transform (letterboxing, integer scaling, clip space).

#include "se_render_core/sprite_vertices.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render/alloc_counter.h"
#include "se_core/types.h"
#include "se_render_core/render_list.h"

namespace se {
namespace {

SpriteQuad MakeQuad(float x, float y, std::uint8_t tint) {
  SpriteQuad quad;
  quad.pos = {Vec2{x, y}, Vec2{x + 4, y}, Vec2{x + 4, y + 2}, Vec2{x, y + 2}};
  quad.uv = {Vec2{0, 0}, Vec2{16, 0}, Vec2{16, 8}, Vec2{0, 8}};
  quad.tint = Rgba8{tint, 2, 3, 4};
  return quad;
}

TEST_CASE("Sprite vertices keep the corner order, texels and tint",
          "[vertices]") {
  const std::vector<SpriteQuad> quads = {MakeQuad(10, 20, 1),
                                         MakeQuad(-5, 7, 9)};
  std::vector<SpriteVertex> vertices(quads.size() * kVerticesPerQuad);
  WriteSpriteVertices(quads, vertices);
  for (std::size_t q = 0; q < quads.size(); ++q) {
    for (std::size_t corner = 0; corner < 4; ++corner) {
      const SpriteVertex& vertex = vertices[q * 4 + corner];
      CHECK(vertex.x == quads[q].pos[corner].x);
      CHECK(vertex.y == quads[q].pos[corner].y);
      CHECK(vertex.u == quads[q].uv[corner].x);
      CHECK(vertex.v == quads[q].uv[corner].y);
      CHECK(vertex.tint == quads[q].tint);
    }
  }
}

TEST_CASE("Quad indices are two triangles per quad", "[vertices]") {
  std::vector<std::uint32_t> indices(std::size_t{3} * kIndicesPerQuad);
  WriteQuadIndices(indices);
  const std::vector<std::uint32_t> expected = {0, 1, 2, 0, 2, 3,  4, 5,  6,
                                               4, 6, 7, 8, 9, 10, 8, 10, 11};
  REQUIRE(indices == expected);
}

TEST_CASE("Writing vertices allocates nothing", "[vertices]") {
  const std::vector<SpriteQuad> quads(1000, MakeQuad(1, 2, 3));
  std::vector<SpriteVertex> vertices(quads.size() * kVerticesPerQuad);
  const std::uint64_t before = testing::AllocationCount();
  WriteSpriteVertices(quads, vertices);
  if (testing::CountingAllocations()) {
    CHECK(testing::AllocationCount() == before);
  }
  CHECK(vertices.back().tint == Rgba8{3, 2, 3, 4});
}

TEST_CASE("A canvas of the target's size maps one to one", "[canvas]") {
  const CanvasFit fit = FitCanvas({1920, 1080}, false, 1920, 1080);
  CHECK(fit.scale == 1.0F);
  CHECK(fit.offset == Vec2{0, 0});
  const std::array<float, 4> clip = CanvasToClip(fit, 1920, 1080);
  // Pixel 0 is clip -1 (left) and +1 (top); pixel 1920 is +1, pixel 1080
  // is -1.
  CHECK(clip[2] == -1.0F);
  CHECK(std::abs(clip[0] * 1920.0F + clip[2] - 1.0F) < 1e-6F);
  CHECK(clip[3] == 1.0F);
  CHECK(std::abs(clip[1] * 1080.0F + clip[3] + 1.0F) < 1e-6F);
}

TEST_CASE("A wider target letterboxes the canvas left and right", "[canvas]") {
  const CanvasFit fit = FitCanvas({1920, 1080}, false, 1000, 400);
  CHECK(fit.scale == 400.0F / 1080.0F);
  // 1920 * 0.37 = 711.1: 288.9 px of bars, 144 on each side.
  CHECK(fit.offset.x == 144.0F);
  CHECK(fit.offset.y == 0.0F);
}

TEST_CASE("Integer scaling uses whole multiples when the canvas fits",
          "[canvas]") {
  const CanvasFit doubled = FitCanvas({320, 180}, true, 1000, 600);
  CHECK(doubled.scale == 3.0F);
  CHECK(doubled.offset == Vec2{20, 30});
  // Smaller than the canvas: scaled down without the integer rule.
  const CanvasFit smaller = FitCanvas({320, 180}, true, 160, 90);
  CHECK(smaller.scale == 0.5F);
  // An empty canvas or target leaves the identity.
  CHECK(FitCanvas({0, 0}, false, 100, 100).scale == 1.0F);
  CHECK(FitCanvas({100, 100}, false, 0, 100).scale == 1.0F);
}

}  // namespace
}  // namespace se
