// Copyright 2026 The simple-engine-2 Authors.

#include "se_render_core/render_list.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <optional>
#include <utility>
#include <vector>

#include "se_core/core_components.h"
#include "se_core/detmath.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"

namespace se {
namespace {

using SortItem = RenderListScratch::SortItem;

constexpr float kDegreesToRadians = static_cast<float>(dm::kPi / 180.0);

// The shorter signed arc from |from| to |to| in degrees, in [-180, 180): a
// half turn goes the negative way. Most ticks turn by less than half a
// turn, which skips the floor.
float ShortestArc(float from, float to) noexcept {
  const float d = to - from;
  if (d >= -180.0F && d < 180.0F) return d;
  return d - 360.0F * std::floor((d + 180.0F) / 360.0F);
}

// A 32-bit key that orders floats as their values do: negative values have
// every bit flipped, others only the sign bit. -0 is folded into +0 first;
// NaNs sort after +inf (positive) or before -inf (negative).
std::uint32_t FloatKey(float value) noexcept {
  const float folded = value == 0.0F ? 0.0F : value;
  const auto bits = std::bit_cast<std::uint32_t>(folded);
  const std::uint32_t mask =
      (bits & 0x80000000U) != 0 ? 0xFFFFFFFFU : 0x80000000U;
  return bits ^ mask;
}

// Sorts items[0, n) by (hi, lo) with a least-significant-digit radix sort
// over the 14 key bytes that can differ (the layer key uses 16 of hi's
// upper 32 bits). Bytes equal in every item (zero in |lo_varying| and
// |hi_varying|) are skipped, so a list on one layer with few textures sorts
// in a handful of passes. Stable, and O(n) per pass. |swap| holds at least
// n items. Returns the buffer that holds the result.
SortItem* RadixSort(SortItem* items, SortItem* swap, std::size_t n,
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

}  // namespace

void BuildRenderList(const World& world, const Camera2D& camera, float alpha,
                     RenderList& list) {
  RenderListScratch& scratch = list.scratch;
  list.stats = RenderStats{};

  const entt::registry& reg = world.CReg();
  const auto* sprites = reg.storage<SpriteRenderer>();
  const auto* transforms = reg.storage<Transform>();
  const auto* spawned = reg.storage<Spawned>();
  const auto* prevs = reg.storage<PrevTransform>();
  if (sprites == nullptr || transforms == nullptr || spawned == nullptr ||
      prevs == nullptr || !(camera.zoom > 0.0F)) {
    list.quads.clear();
    list.batches.clear();
    return;
  }

  // NaN draws the Transform (alpha 1).
  float a = 1.0F;
  if (alpha >= 0.0F && alpha < 1.0F) a = alpha;
  if (alpha < 0.0F) a = 0.0F;
  const float b = 1.0F - a;

  const Tick now = world.Clock().tick;
  const float zoom = camera.zoom;
  const float width = camera.virtual_size.x;
  const float height = camera.virtual_size.y;
  const float half_w = 0.5F * width;
  const float half_h = 0.5F * height;

  // Visible sprites take the next quad slot and sort item. The buffers only
  // grow, so a warm list never allocates.
  const std::size_t capacity = sprites->size();
  if (scratch.quads.size() < capacity) scratch.quads.resize(capacity);
  if (scratch.items.size() < capacity) scratch.items.resize(capacity);
  if (scratch.items_swap.size() < capacity) {
    scratch.items_swap.resize(capacity);
  }
  list.quads.reserve(capacity);
  // At most one batch per quad.
  list.batches.reserve(capacity);
  SpriteQuad* out = scratch.quads.data();
  SortItem* items = scratch.items.data();
  std::uint32_t visible = 0;
  std::uint32_t interpolated = 0;
  std::uint32_t candidates = 0;
  // Bits that differ between keys, so the sort can skip uniform bytes.
  std::uint64_t lo_or = 0;
  std::uint64_t lo_and = ~std::uint64_t{0};
  std::uint64_t hi_or = 0;
  std::uint64_t hi_and = ~std::uint64_t{0};

  for (const auto [entity, sprite] : sprites->each()) {
    if (!transforms->contains(entity) || !spawned->contains(entity)) continue;
    const Transform& transform = transforms->get(entity);
    const Spawned& spawn = spawned->get(entity);
    ++candidates;
    Vec2 pos = transform.pos;
    float rot = transform.rot;
    Vec2 scale = transform.scale;
    if (transform.teleport_tick != now && prevs->contains(entity)) {
      const PrevTransform& prev = prevs->get(entity);
      if (prev.tick + 1 == now) {
        // Exact at both ends: alpha 1 gives the Transform, 0 the previous.
        pos = pos * a + prev.pos * b;
        scale = scale * a + prev.scale * b;
        rot -= ShortestArc(prev.rot, rot) * b;
        ++interpolated;
      }
    }

    // The sprite's local rectangle around its origin, mirrored about the
    // origin by the flips, then scaled into canvas pixels.
    const Rect& region = sprite.region;
    const Vec2 origin = sprite.origin;
    const float x0 =
        (sprite.flip_x ? origin.x - region.w : -origin.x) * (scale.x * zoom);
    const float x1 =
        (sprite.flip_x ? origin.x : region.w - origin.x) * (scale.x * zoom);
    const float y0 =
        (sprite.flip_y ? origin.y - region.h : -origin.y) * (scale.y * zoom);
    const float y1 =
        (sprite.flip_y ? origin.y : region.h - origin.y) * (scale.y * zoom);
    const float u_left = region.x;
    const float u_right = region.x + region.w;
    const float v_top = region.y;
    const float v_bottom = region.y + region.h;

    // Rotate (clockwise on screen for positive degrees, y down) and move to
    // the canvas: corner (x, y) -> (cx + x c - y s, cy + x s + y c).
    const dm::SinCosPair<float> sc = dm::SinCos(rot * kDegreesToRadians);
    const float cx = (pos.x - camera.center.x) * zoom + half_w;
    const float cy = (pos.y - camera.center.y) * zoom + half_h;
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
    const bool on_canvas =
        max_x > 0.0F && min_x < width && max_y > 0.0F && min_y < height;
    if (!on_canvas) continue;

    SpriteQuad& quad = out[visible];
    quad.pos[0] = {cx + x0c - y0s, cy + x0s + y0c};
    quad.pos[1] = {cx + x1c - y0s, cy + x1s + y0c};
    quad.pos[2] = {cx + x1c - y1s, cy + x1s + y1c};
    quad.pos[3] = {cx + x0c - y1s, cy + x0s + y1c};
    quad.uv[0] = {sprite.flip_x ? u_right : u_left,
                  sprite.flip_y ? v_bottom : v_top};
    quad.uv[1] = {sprite.flip_x ? u_left : u_right, quad.uv[0].y};
    quad.uv[2] = {quad.uv[1].x, sprite.flip_y ? v_top : v_bottom};
    quad.uv[3] = {quad.uv[0].x, quad.uv[2].y};
    quad.tint = sprite.tint;

    const auto layer_key = static_cast<std::uint64_t>(
        static_cast<std::uint16_t>(sprite.layer) ^ 0x8000U);
    SortItem& item = items[visible];
    item.lo = (static_cast<std::uint64_t>(FloatKey(sprite.z)) << 32) |
              spawn.spawn_serial;
    item.hi = (layer_key << 32) | sprite.texture;
    item.quad = visible;
    lo_or |= item.lo;
    lo_and &= item.lo;
    hi_or |= item.hi;
    hi_and &= item.hi;
    ++visible;
  }

  const SortItem* sorted = RadixSort(items, scratch.items_swap.data(), visible,
                                     lo_or ^ lo_and, hi_or ^ hi_and);

  // Gather the quads in draw order and cut batches where the texture
  // changes.
  list.quads.resize(visible);
  list.batches.clear();
  SpriteQuad* quads = list.quads.data();
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

std::optional<Camera2D> PrimaryCamera(const World& world) {
  const entt::registry& reg = world.CReg();
  std::optional<Camera2D> best;
  std::uint32_t best_serial = 0;
  for (const auto [entity, camera, spawn] :
       reg.view<Camera2D, Spawned>().each()) {
    if (!camera.primary) continue;
    if (!best || spawn.spawn_serial < best_serial) {
      best = camera;
      best_serial = spawn.spawn_serial;
    }
  }
  return best;
}

}  // namespace se
