// Copyright 2026 The simple-engine-2 Authors.
//
// Viewport picking (docs/design/mt-design.md "Editor": "Picking uses the
// broadphase scratch when its validTick is current, then exact AABBs
// ordered by layer and z. A faulted World picks by brute force over
// registry storages"; ADR-101).
//
// A sprite's pick box is the axis-aligned bounds of the quad the renderer
// draws for it (the per-axis extremes of its corners, computed as
// BuildRenderList computes them, at zoom 1 in world pixels), and a point
// hits it when min <= p < max. Of the boxes a point hits, the one drawn last
// wins: the largest (layer, texture, z, spawn serial), the RenderList's draw
// order, so the pick is the sprite on top of what the viewport shows.
//
// Three paths, all giving the same answer on a consistent World:
//   - kBroadphase: a PickScratch built for the World's current tick (a
//     Play World at the hash its last tick recorded, unchanged since: not
//     HashDirty) answers from a uniform grid of the boxes;
//   - kExact: otherwise every entity with a Transform, a SpriteRenderer and
//     a Spawned record is tested (Edit documents always take this path:
//     they are never stepped, and their sprites are not hashed);
//   - kBruteForce: a faulted World (frozen after an exception, Play.Faulted)
//     is walked through the IdComponent storage with try_get only, so a
//     World a throw left half-updated is never iterated through a multi-
//     component view.

#ifndef SE_EDITOR_PANELS_PICKING_H_
#define SE_EDITOR_PANELS_PICKING_H_

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <vector>

#include "game_rampage/broadphase.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"

namespace se::editor {

struct PickBounds {
  Vec2 min;
  Vec2 max;
};

// The world-space bounds of the sprite's quad.
[[nodiscard]] PickBounds SpriteBounds(const Transform& transform,
                                      const SpriteRenderer& sprite) noexcept;

// min <= point < max on both axes (NaN bounds contain nothing).
[[nodiscard]] bool BoundsContain(const PickBounds& bounds, Vec2 point) noexcept;

enum class PickPath : std::uint8_t { kBroadphase, kExact, kBruteForce };

struct PickHit {
  entt::entity entity = entt::null;
  Guid guid;
  PickBounds bounds;
};

struct PickResult {
  // Topmost first.
  std::vector<PickHit> hits;
  PickPath path = PickPath::kExact;

  [[nodiscard]] bool Empty() const noexcept { return hits.empty(); }
};

// The broadphase scratch: every pick box of one World at one tick.
class PickScratch {
 public:
  PickScratch() = default;

  // Rebuilds the scratch when |world| is at a recorded tick it was not
  // built for (see the header comment); otherwise leaves it alone.
  void Refresh(const World& world);
  // Whether the scratch describes |world| as it is now.
  [[nodiscard]] bool ValidFor(const World& world) const noexcept;
  // Rebuilds counted, for tests.
  [[nodiscard]] std::uint64_t Builds() const noexcept { return builds_; }

 private:
  friend PickResult Pick(const World& world, Vec2 point, bool faulted,
                         PickScratch* scratch);

  struct Box {
    entt::entity entity = entt::null;
    Guid guid;
    PickBounds bounds;
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
  };

  const World* world_ = nullptr;
  Tick tick_ = 0;
  std::uint64_t hash_ = 0;
  bool valid_ = false;
  std::uint64_t builds_ = 0;
  std::vector<Box> boxes_;
  std::vector<rampage::Aabb> aabbs_;
  rampage::SpatialHash grid_;
  std::vector<std::uint32_t> query_;
};

// Whether |world| is in a state a PickScratch may describe: not an Edit
// document, its last tick's hash recorded and nothing changed since.
[[nodiscard]] bool WorldAtRecordedTick(const World& world) noexcept;

// Every sprite under |point| (world pixels), topmost first, by the path the
// header comment describes. |scratch| may be null (no broadphase).
[[nodiscard]] PickResult Pick(const World& world, Vec2 point, bool faulted,
                              PickScratch* scratch);

}  // namespace se::editor

#endif  // SE_EDITOR_PANELS_PICKING_H_
