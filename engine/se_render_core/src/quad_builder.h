// Copyright 2026 The simple-engine-2 Authors.
//
// The part of a RenderList build that does not depend on where the sprites
// come from (internal to se_render_core): a sprite's resolved pose and look
// become its quad in canvas pixels, culled against the canvas, with a sort
// key (EmitQuad); FinishList sorts the quads by (layer, texture, z, spawn
// serial) and cuts the batches. BuildRenderList (a World, render_list.cc) and
// BuildSnapshotRenderList (two snapshot slots, snapshot_render_source.cc)
// both resolve each sprite and hand it here, so for the same poses they
// produce the same quads bit for bit (docs/milestone-specs.md M17a).
//
// Everything is inline: the per-sprite call sits in each builder's hot loop,
// whose counters stay locals of the builder (MSVC does no type-based alias
// analysis, so counters kept in an object would be reloaded after every
// quad store).

#ifndef ENGINE_SE_RENDER_CORE_SRC_QUAD_BUILDER_H_
#define ENGINE_SE_RENDER_CORE_SRC_QUAD_BUILDER_H_

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "se_core/detmath.h"
#include "se_core/types.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"

// The per-sprite step is too large for the compilers' own inlining budget;
// a call per sprite costs about 5% of a 10k-sprite build.
#if defined(_MSC_VER) && !defined(__clang__)
#define SE_QUAD_INLINE __forceinline
#else
#define SE_QUAD_INLINE inline __attribute__((always_inline))
#endif

namespace se::render_internal {

inline constexpr float kHalfTurn = static_cast<float>(dm::kPi);
inline constexpr float kTurn = static_cast<float>(2.0 * dm::kPi);

// The shorter signed arc from |from| to |to| in radians, in [-pi, pi): a
// half turn goes the negative way. Most ticks turn by less than half a
// turn, which skips the floor.
inline float ShortestArc(float from, float to) noexcept {
  const float d = to - from;
  if (d >= -kHalfTurn && d < kHalfTurn) return d;
  return d - kTurn * std::floor((d + kHalfTurn) / kTurn);
}

// The interpolation factor a build uses: |alpha| clamped to [0, 1], NaN
// drawing the newer state (1).
inline float ClampAlpha(float alpha) noexcept {
  float a = 1.0F;
  if (alpha >= 0.0F && alpha < 1.0F) a = alpha;
  if (alpha < 0.0F) a = 0.0F;
  return a;
}

// A 32-bit key that orders floats as their values do: negative values have
// every bit flipped, others only the sign bit. -0 is folded into +0 first;
// NaNs sort after +inf (positive) or before -inf (negative).
inline std::uint32_t FloatKey(float value) noexcept {
  const float folded = value == 0.0F ? 0.0F : value;
  const auto bits = std::bit_cast<std::uint32_t>(folded);
  const std::uint32_t mask =
      (bits & 0x80000000U) != 0 ? 0xFFFFFFFFU : 0x80000000U;
  return bits ^ mask;
}

using SortItem = RenderListScratch::SortItem;

// Sorts items[0, n) by (hi, lo) with a least-significant-digit radix sort
// over the 14 key bytes that can differ (the layer key uses 16 of hi's
// upper 32 bits). Bytes equal in every item (zero in |lo_varying| and
// |hi_varying|) are skipped, so a list on one layer with few textures sorts
// in a handful of passes. Stable, and O(n) per pass. |swap| holds at least
// n items. Returns the buffer that holds the result.
inline SortItem* RadixSort(SortItem* items, SortItem* swap, std::size_t n,
                           std::uint64_t lo_varying, std::uint64_t hi_varying) {
  constexpr int kLoBytes = 8;
  constexpr int kHiBytes = 6;
  SortItem* from = items;
  SortItem* to = swap;
  if (n < 2) return from;
  for (int p = 0; p < kLoBytes + kHiBytes; ++p) {
    const bool use_hi = p >= kLoBytes;
    const int shift = 8 * (use_hi ? p - kLoBytes : p);
    const std::uint64_t varying = use_hi ? hi_varying : lo_varying;
    if (((varying >> shift) & 0xFFU) == 0) continue;

    std::array<std::uint32_t, 256> offsets{};
    for (std::size_t k = 0; k < n; ++k) {
      const std::uint64_t word = use_hi ? from[k].hi : from[k].lo;
      ++offsets[(word >> shift) & 0xFFU];
    }
    std::uint32_t sum = 0;
    for (std::uint32_t& offset : offsets) {
      const std::uint32_t count = offset;
      offset = sum;
      sum += count;
    }
    for (std::size_t k = 0; k < n; ++k) {
      const std::uint64_t word = use_hi ? from[k].hi : from[k].lo;
      to[offsets[(word >> shift) & 0xFFU]++] = from[k];
    }
    std::swap(from, to);
  }
  return from;
}

// A sprite's pose as a build resolved it (after interpolation).
struct Pose {
  Vec2 pos;
  float rot = 0.0F;
  Vec2 scale{1.0F, 1.0F};
};

// The rest of what EmitQuad draws, beside the look's region, origin,
// texture, layer and z.
struct Mark {
  Rgba8 tint;
  bool flip_x = false;
  bool flip_y = false;
  std::uint32_t spawn_serial = 0;
};

// The camera's mapping of world pixels to canvas pixels.
struct Canvas {
  float zoom = 1.0F;
  float width = 0.0F;
  float height = 0.0F;
  float half_w = 0.0F;
  float half_h = 0.0F;
  Vec2 center;
};

inline Canvas CanvasOf(const Camera2D& camera) noexcept {
  Canvas canvas;
  canvas.zoom = camera.zoom;
  canvas.width = camera.virtual_size.x;
  canvas.height = camera.virtual_size.y;
  canvas.half_w = 0.5F * canvas.width;
  canvas.half_h = 0.5F * canvas.height;
  canvas.center = camera.center;
  return canvas;
}

// Sizes |list|'s buffers for at most |capacity| sprites and clears its
// stats; returns its scratch. The buffers only grow, so a warm list never
// allocates.
inline RenderListScratch& PrepareList(RenderList& list, std::size_t capacity) {
  RenderListScratch& scratch = list.scratch;
  list.stats = RenderStats{};
  if (scratch.quads.size() < capacity) scratch.quads.resize(capacity);
  if (scratch.items.size() < capacity) scratch.items.resize(capacity);
  if (scratch.items_swap.size() < capacity) {
    scratch.items_swap.resize(capacity);
  }
  list.quads.reserve(capacity);
  // At most one batch per quad.
  list.batches.reserve(capacity);
  return scratch;
}

// The bits that differ between the sort keys, so the sort can skip uniform
// bytes.
struct KeyBits {
  std::uint64_t lo_or = 0;
  std::uint64_t lo_and = ~std::uint64_t{0};
  std::uint64_t hi_or = 0;
  std::uint64_t hi_and = ~std::uint64_t{0};

  void Add(const SortItem& item) noexcept {
    lo_or |= item.lo;
    lo_and &= item.lo;
    hi_or |= item.hi;
    hi_and &= item.hi;
  }
};

// Writes the quad in canvas pixels of a sprite at |pose| that looks like
// |look| (a SpriteRenderer or a RenderItem: its region, origin, texture,
// layer and z) with |mark|'s tint, flips and spawn serial, and its sort
// item (quad index |index|); false when the sprite is outside the canvas
// (then neither is meaningful). The look is read in place, never copied,
// and the builders keep their counters in locals around this call, so the
// hot loop stays in registers.
template <class Look>
SE_QUAD_INLINE bool EmitQuad(const Pose& pose, const Look& look,
                             const Mark& mark, const Canvas& canvas,
                             std::uint32_t index, SpriteQuad& quad,
                             SortItem& item) noexcept {
  // The sprite's local rectangle around its origin, mirrored about the
  // origin by the flips, then scaled into canvas pixels.
  const Rect& region = look.region;
  const Vec2 origin = look.origin;
  const Vec2 scale = pose.scale;
  const bool flip_x = mark.flip_x;
  const bool flip_y = mark.flip_y;
  const float zoom = canvas.zoom;
  const float x0 =
      (flip_x ? origin.x - region.w : -origin.x) * (scale.x * zoom);
  const float x1 = (flip_x ? origin.x : region.w - origin.x) * (scale.x * zoom);
  const float y0 =
      (flip_y ? origin.y - region.h : -origin.y) * (scale.y * zoom);
  const float y1 = (flip_y ? origin.y : region.h - origin.y) * (scale.y * zoom);
  const float u_left = region.x;
  const float u_right = region.x + region.w;
  const float v_top = region.y;
  const float v_bottom = region.y + region.h;

  // Rotate (clockwise on screen for positive angles, y down) and move to
  // the canvas: corner (x, y) -> (cx + x c - y s, cy + x s + y c).
  const dm::SinCosPair<float> sc = dm::SinCos(pose.rot);
  const float cx = (pose.pos.x - canvas.center.x) * zoom + canvas.half_w;
  const float cy = (pose.pos.y - canvas.center.y) * zoom + canvas.half_h;
  const float x0c = x0 * sc.cos;
  const float x1c = x1 * sc.cos;
  const float x0s = x0 * sc.sin;
  const float x1s = x1 * sc.sin;
  const float y0c = y0 * sc.cos;
  const float y1c = y1 * sc.cos;
  const float y0s = y0 * sc.sin;
  const float y1s = y1 * sc.sin;

  // Culled unless the rotated bounds (the per-axis extremes of the
  // corners) overlap the canvas with a positive area. The branch is cheap
  // even when mispredicted, and unlike a branch-free count it does not
  // make every iteration wait for the previous one's result.
  const float min_x = cx + std::min(x0c, x1c) - std::max(y0s, y1s);
  const float max_x = cx + std::max(x0c, x1c) - std::min(y0s, y1s);
  const float min_y = cy + std::min(x0s, x1s) + std::min(y0c, y1c);
  const float max_y = cy + std::max(x0s, x1s) + std::max(y0c, y1c);
  // Written so that NaN bounds are culled too.
  const bool on_canvas = max_x > 0.0F && min_x < canvas.width && max_y > 0.0F &&
                         min_y < canvas.height;
  if (!on_canvas) return false;

  quad.pos[0] = {cx + x0c - y0s, cy + x0s + y0c};
  quad.pos[1] = {cx + x1c - y0s, cy + x1s + y0c};
  quad.pos[2] = {cx + x1c - y1s, cy + x1s + y1c};
  quad.pos[3] = {cx + x0c - y1s, cy + x0s + y1c};
  quad.uv[0] = {flip_x ? u_right : u_left, flip_y ? v_bottom : v_top};
  quad.uv[1] = {flip_x ? u_left : u_right, quad.uv[0].y};
  quad.uv[2] = {quad.uv[1].x, flip_y ? v_top : v_bottom};
  quad.uv[3] = {quad.uv[0].x, quad.uv[2].y};
  quad.tint = mark.tint;

  const auto layer_key = static_cast<std::uint64_t>(
      static_cast<std::uint16_t>(look.layer) ^ 0x8000U);
  item.lo =
      (static_cast<std::uint64_t>(FloatKey(look.z)) << 32) | mark.spawn_serial;
  item.hi = (layer_key << 32) | look.texture;
  item.quad = index;
  return true;
}

// Sorts the |visible| quads EmitQuad wrote into |list|'s scratch, gathers
// them in draw order, cuts the batches and fills the stats: |candidates|
// sprites were considered, |interpolated| of them drawn between two states.
inline void FinishList(RenderList& list, std::uint32_t visible,
                       const KeyBits& bits, std::uint32_t candidates,
                       std::uint32_t interpolated) {
  RenderListScratch& scratch = list.scratch;
  const SortItem* sorted =
      RadixSort(scratch.items.data(), scratch.items_swap.data(), visible,
                bits.lo_or ^ bits.lo_and, bits.hi_or ^ bits.hi_and);

  // Gather the quads in draw order and cut batches where the texture
  // changes.
  list.quads.resize(visible);
  list.batches.clear();
  SpriteQuad* quads = list.quads.data();
  const SpriteQuad* out = scratch.quads.data();
  for (std::uint32_t k = 0; k < visible; ++k) {
    const SortItem& item = sorted[k];
    quads[k] = out[item.quad];
    const auto texture = static_cast<std::uint32_t>(item.hi);
    if (list.batches.empty() || list.batches.back().texture != texture) {
      list.batches.push_back(QuadBatch{texture, k, 0});
    }
    ++list.batches.back().count;
  }

  RenderStats& stats = list.stats;
  stats.sprites = candidates;
  stats.interpolated = interpolated;
  stats.culled = candidates - visible;
  stats.quads = visible;
  stats.batches = static_cast<std::uint32_t>(list.batches.size());
  stats.quad_bytes = static_cast<std::uint64_t>(visible) * sizeof(SpriteQuad);
}

}  // namespace se::render_internal

#endif  // ENGINE_SE_RENDER_CORE_SRC_QUAD_BUILDER_H_
