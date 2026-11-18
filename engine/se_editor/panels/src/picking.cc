// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor_panels/picking.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <vector>

#include "game_rampage/broadphase.h"
#include "se_core/core_components.h"
#include "se_core/detmath.h"
#include "se_core/guid.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"

namespace se::editor {

PickBounds SpriteBounds(const Transform& transform,
                        const SpriteRenderer& sprite) noexcept {
  // EmitQuad's corners (se_render_core/src/quad_builder.h) at zoom 1, in
  // world pixels: the local rectangle around the origin, mirrored by the
  // flips, scaled, rotated about the origin and moved to the position.
  const Rect& region = sprite.region;
  const Vec2 origin = sprite.origin;
  const Vec2 scale = transform.scale;
  const float x0 = (sprite.flip_x ? origin.x - region.w : -origin.x) * scale.x;
  const float x1 = (sprite.flip_x ? origin.x : region.w - origin.x) * scale.x;
  const float y0 = (sprite.flip_y ? origin.y - region.h : -origin.y) * scale.y;
  const float y1 = (sprite.flip_y ? origin.y : region.h - origin.y) * scale.y;
  const dm::SinCosPair<float> sc = dm::SinCos(transform.rot);
  const float x0c = x0 * sc.cos;
  const float x1c = x1 * sc.cos;
  const float x0s = x0 * sc.sin;
  const float x1s = x1 * sc.sin;
  const float y0c = y0 * sc.cos;
  const float y1c = y1 * sc.cos;
  const float y0s = y0 * sc.sin;
  const float y1s = y1 * sc.sin;
  const Vec2 pos = transform.pos;
  return PickBounds{{pos.x + std::min(x0c, x1c) - std::max(y0s, y1s),
                     pos.y + std::min(x0s, x1s) + std::min(y0c, y1c)},
                    {pos.x + std::max(x0c, x1c) - std::min(y0s, y1s),
                     pos.y + std::max(x0s, x1s) + std::max(y0c, y1c)}};
}

bool BoundsContain(const PickBounds& bounds, Vec2 point) noexcept {
  return point.x >= bounds.min.x && point.x < bounds.max.x &&
         point.y >= bounds.min.y && point.y < bounds.max.y;
}

namespace {

// A float's order as an unsigned key; -0 is +0 (BuildRenderList's rule).
std::uint32_t FloatOrder(float value) noexcept {
  const float folded = value == 0.0F ? 0.0F : value;
  const auto bits = std::bit_cast<std::uint32_t>(folded);
  const std::uint32_t mask =
      (bits & 0x80000000U) != 0 ? 0xFFFFFFFFU : 0x80000000U;
  return bits ^ mask;
}

// The RenderList's sort key (layer, texture, z, spawn serial) as two words.
void DrawKey(const SpriteRenderer& sprite, std::uint32_t serial,
             std::uint64_t& hi, std::uint64_t& lo) noexcept {
  const auto layer_key = static_cast<std::uint64_t>(
      static_cast<std::uint16_t>(sprite.layer) ^ 0x8000U);
  hi = (layer_key << 32) | sprite.texture;
  lo = (static_cast<std::uint64_t>(FloatOrder(sprite.z)) << 32) | serial;
}

struct Candidate {
  PickHit hit;
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
};

void SortTopmostFirst(std::vector<Candidate>& candidates, PickResult& result) {
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.hi != b.hi ? a.hi > b.hi : a.lo > b.lo;
            });
  result.hits.clear();
  result.hits.reserve(candidates.size());
  for (const Candidate& c : candidates) result.hits.push_back(c.hit);
}

Guid GuidOf(const entt::registry& reg, entt::entity e) {
  const auto* id = reg.try_get<IdComponent>(e);
  return id != nullptr ? id->guid : Guid{};
}

}  // namespace

bool WorldAtRecordedTick(const World& world) noexcept {
  return world.Desc().kind != WorldKind::kEditDocument && !world.HashDirty() &&
         world.LastHash().tick == world.Clock().tick;
}

bool PickScratch::ValidFor(const World& world) const noexcept {
  return valid_ && world_ == &world && WorldAtRecordedTick(world) &&
         tick_ == world.Clock().tick && hash_ == world.LastHash().Folded();
}

void PickScratch::Refresh(const World& world) {
  if (ValidFor(world)) return;
  valid_ = false;
  if (!WorldAtRecordedTick(world)) return;
  const entt::registry& reg = world.CReg();
  boxes_.clear();
  aabbs_.clear();
  for (const auto [e, transform, sprite, spawned] :
       reg.view<const Transform, const SpriteRenderer, const Spawned>()
           .each()) {
    Box box;
    box.entity = e;
    box.guid = GuidOf(reg, e);
    box.bounds = SpriteBounds(transform, sprite);
    DrawKey(sprite, spawned.spawn_serial, box.hi, box.lo);
    boxes_.push_back(box);
    aabbs_.push_back(rampage::Aabb{box.bounds.min, box.bounds.max});
  }
  grid_.Build(aabbs_);
  world_ = &world;
  tick_ = world.Clock().tick;
  hash_ = world.LastHash().Folded();
  valid_ = true;
  ++builds_;
}

PickResult Pick(const World& world, Vec2 point, bool faulted,
                PickScratch* scratch) {
  PickResult result;
  std::vector<Candidate> candidates;
  const entt::registry& reg = world.CReg();
  if (faulted) {
    result.path = PickPath::kBruteForce;
    // One storage, then try_get: nothing assumes the components agree.
    for (const auto [e, id] : reg.view<const IdComponent>().each()) {
      const auto* transform = reg.try_get<Transform>(e);
      const auto* sprite = reg.try_get<SpriteRenderer>(e);
      if (transform == nullptr || sprite == nullptr) continue;
      const PickBounds bounds = SpriteBounds(*transform, *sprite);
      if (!BoundsContain(bounds, point)) continue;
      const auto* spawned = reg.try_get<Spawned>(e);
      Candidate c{PickHit{e, id.guid, bounds}};
      DrawKey(*sprite, spawned != nullptr ? spawned->spawn_serial : 0, c.hi,
              c.lo);
      candidates.push_back(c);
    }
  } else if (scratch != nullptr && scratch->ValidFor(world)) {
    result.path = PickPath::kBroadphase;
    scratch->grid_.Query(rampage::Aabb{point, point}, scratch->query_);
    for (const std::uint32_t index : scratch->query_) {
      const PickScratch::Box& box = scratch->boxes_[index];
      if (!BoundsContain(box.bounds, point)) continue;
      candidates.push_back(
          Candidate{PickHit{box.entity, box.guid, box.bounds}, box.hi, box.lo});
    }
  } else {
    result.path = PickPath::kExact;
    for (const auto [e, transform, sprite, spawned] :
         reg.view<const Transform, const SpriteRenderer, const Spawned>()
             .each()) {
      const PickBounds bounds = SpriteBounds(transform, sprite);
      if (!BoundsContain(bounds, point)) continue;
      Candidate c{PickHit{e, GuidOf(reg, e), bounds}};
      DrawKey(sprite, spawned.spawn_serial, c.hi, c.lo);
      candidates.push_back(c);
    }
  }
  SortTopmostFirst(candidates, result);
  return result;
}

}  // namespace se::editor
