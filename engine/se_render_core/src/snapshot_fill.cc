// Copyright 2026 The simple-engine-2 Authors.

#include "se_render_core/snapshot_fill.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "se_core/core_components.h"
#include "se_core/snapshot.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"

namespace se {

std::uint32_t SnapshotEntityIndex(std::uint32_t entity) noexcept {
  return static_cast<std::uint32_t>(
      entt::to_entity(static_cast<entt::entity>(entity)));
}

bool AppendRenderView(const World& world, const RenderViewOptions& options,
                      Snapshot& slot) {
  if (slot.view_count >= kMaxSnapshotViews) return false;
  SnapshotView& view = slot.views[slot.view_count++];
  view = SnapshotView{};
  const Camera2D& camera = options.camera;
  view.camera.center = camera.center;
  view.camera.zoom = camera.zoom;
  view.camera.virtual_size = camera.virtual_size;
  view.camera.integer_scale = camera.integer_scale;
  view.camera.primary = camera.primary;
  view.camera.cut = options.camera_cut;
  view.flags = options.stepped ? kViewStepped : std::uint8_t{0};
  view.first_item = static_cast<std::uint32_t>(slot.items.size());
  view.first_sparse = static_cast<std::uint32_t>(slot.sparse.size());

  const entt::registry& reg = world.CReg();
  const auto* sprites = reg.storage<SpriteRenderer>();
  const auto* transforms = reg.storage<Transform>();
  const auto* spawned = reg.storage<Spawned>();
  const auto* prevs = reg.storage<PrevTransform>();
  if (sprites == nullptr || transforms == nullptr || spawned == nullptr) {
    return true;
  }
  const Tick now = world.Clock().tick;
  const float limit = options.teleport_distance;
  const float limit_squared = limit * limit;
  std::uint32_t max_index = 0;
  slot.items.reserve(slot.items.size() + sprites->size());
  for (const auto [entity, sprite] : sprites->each()) {
    if (!transforms->contains(entity) || !spawned->contains(entity)) continue;
    const Transform& transform = transforms->get(entity);
    RenderItem item;
    item.entity = entt::to_integral(entity);
    item.spawn_serial = spawned->get(entity).spawn_serial;
    item.pos = transform.pos;
    item.rot = transform.rot;
    item.scale = transform.scale;
    item.texture = sprite.texture;
    item.region = sprite.region;
    item.origin = sprite.origin;
    item.tint = sprite.tint;
    item.layer = sprite.layer;
    item.z = sprite.z;
    item.flags = kItemVisible;
    if (sprite.flip_x) item.flags |= kItemFlipX;
    if (sprite.flip_y) item.flags |= kItemFlipY;
    // BuildRenderList's condition for drawing between PrevTransform and
    // Transform, plus the displacement rule.
    bool continuous = false;
    if (transform.teleport_tick != now && prevs != nullptr &&
        prevs->contains(entity)) {
      const PrevTransform& prev = prevs->get(entity);
      if (prev.tick + 1 == now) {
        const Vec2 moved = transform.pos - prev.pos;
        continuous = !(limit > 0.0F) ||
                     moved.x * moved.x + moved.y * moved.y <= limit_squared;
      }
    }
    if (!continuous) item.flags |= kItemTeleported;
    max_index = std::max(max_index, SnapshotEntityIndex(item.entity));
    slot.items.push_back(item);
  }

  view.item_count =
      static_cast<std::uint32_t>(slot.items.size()) - view.first_item;
  if (view.item_count == 0) return true;
  view.sparse_count = max_index + 1;
  slot.sparse.resize(std::size_t{view.first_sparse} + view.sparse_count,
                     kNoSnapshotItem);
  for (std::uint32_t i = view.first_item; i < view.first_item + view.item_count;
       ++i) {
    slot.sparse[view.first_sparse + SnapshotEntityIndex(slot.items[i].entity)] =
        i;
  }
  return true;
}

}  // namespace se
