// Copyright 2026 The simple-engine-2 Authors.

#include "se_render_core/render_list.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <entt/core/type_info.hpp>
#include <entt/entity/registry.hpp>
#include <optional>

#include "se_core/core_components.h"
#include "se_core/jobs.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "src/quad_builder.h"

namespace se {

using render_internal::Canvas;
using render_internal::CanvasOf;
using render_internal::ClampAlpha;
using render_internal::EmitQuad;
using render_internal::FinishList;
using render_internal::KeyBits;
using render_internal::Mark;
using render_internal::Pose;
using render_internal::PrepareList;
using render_internal::ShortestArc;
using render_internal::SortItem;

void BuildRenderList(const World& world, const Camera2D& camera, float alpha,
                     RenderList& list) {
  const entt::registry& reg = world.CReg();
  const auto* sprites = reg.storage<SpriteRenderer>();
  const auto* transforms = reg.storage<Transform>();
  const auto* spawned = reg.storage<Spawned>();
  const auto* prevs = reg.storage<PrevTransform>();
  if (sprites == nullptr || transforms == nullptr || spawned == nullptr ||
      prevs == nullptr || !(camera.zoom > 0.0F)) {
    list.stats = RenderStats{};
    list.quads.clear();
    list.batches.clear();
    return;
  }

  // NaN draws the Transform (alpha 1).
  const float a = ClampAlpha(alpha);
  const float b = 1.0F - a;
  const Tick now = world.Clock().tick;

  const Canvas canvas = CanvasOf(camera);
  RenderListScratch& scratch = PrepareList(list, sprites->size());
  SpriteQuad* out = scratch.quads.data();
  SortItem* items = scratch.items.data();
  KeyBits bits;
  std::uint32_t visible = 0;
  std::uint32_t interpolated = 0;
  std::uint32_t candidates = 0;
  for (const auto [entity, sprite] : sprites->each()) {
    if (!transforms->contains(entity) || !spawned->contains(entity)) continue;
    const Transform& transform = transforms->get(entity);
    const Spawned& spawn = spawned->get(entity);
    ++candidates;
    Pose pose{transform.pos, transform.rot, transform.scale};
    if (transform.teleport_tick != now && prevs->contains(entity)) {
      const PrevTransform& prev = prevs->get(entity);
      if (prev.tick + 1 == now) {
        // Exact at both ends: alpha 1 gives the Transform, 0 the previous.
        pose.pos = pose.pos * a + prev.pos * b;
        pose.scale = pose.scale * a + prev.scale * b;
        pose.rot -= ShortestArc(prev.rot, pose.rot) * b;
        ++interpolated;
      }
    }
    const Mark mark{sprite.tint, sprite.flip_x, sprite.flip_y,
                    spawn.spawn_serial};
    if (EmitQuad(pose, sprite, mark, canvas, visible, out[visible],
                 items[visible])) {
      bits.Add(items[visible]);
      ++visible;
    }
  }
  FinishList(list, visible, bits, candidates, interpolated);
}

const jobs::ForkSiteDecl& RenderListSite() {
  static const std::array<entt::id_type, 4> kReads = {
      entt::type_hash<SpriteRenderer>::value(),
      entt::type_hash<Transform>::value(),
      entt::type_hash<PrevTransform>::value(),
      entt::type_hash<Spawned>::value()};
  static const jobs::ForkSiteDecl kSite{
      .name = "render.RenderList", .grain = 1024, .reads = kReads};
  return kSite;
}

void BuildRenderListForked(const World& world, const Camera2D& camera,
                           float alpha, RenderList& list,
                           jobs::ForkJoinPool& jobs) {
  const entt::registry& reg = world.CReg();
  const auto* sprites = reg.storage<SpriteRenderer>();
  const auto* transforms = reg.storage<Transform>();
  const auto* spawned = reg.storage<Spawned>();
  const auto* prevs = reg.storage<PrevTransform>();
  if (sprites == nullptr || transforms == nullptr || spawned == nullptr ||
      prevs == nullptr || !(camera.zoom > 0.0F)) {
    list.stats = RenderStats{};
    list.quads.clear();
    list.batches.clear();
    return;
  }

  const float a = ClampAlpha(alpha);
  const float b = 1.0F - a;
  const Tick now = world.Clock().tick;
  const Canvas canvas = CanvasOf(camera);
  const auto count = static_cast<std::uint32_t>(sprites->size());
  RenderListScratch& scratch = PrepareList(list, count);
  jobs::ForkSite& site = jobs.Site(RenderListSite());
  scratch.chunks.resize(jobs.ChunkCountFor(site, count));

  // Each chunk reads the storages through const pointers taken here, on
  // the owner (nothing writes the World during the fork), and writes only
  // its own slice of the scratch: quads and items [begin, end).
  jobs.ParallelFor(site, count, [&](jobs::ChunkRange range) {
    SpriteQuad* out = scratch.quads.data() + range.begin;
    SortItem* items = scratch.items.data() + range.begin;
    KeyBits bits;
    std::uint32_t visible = 0;
    std::uint32_t interpolated = 0;
    std::uint32_t candidates = 0;
    const entt::entity* packed = sprites->data();
    for (std::uint32_t i = range.begin; i < range.end; ++i) {
      const entt::entity entity = packed[i];
      if (!transforms->contains(entity) || !spawned->contains(entity)) {
        continue;
      }
      const SpriteRenderer& sprite = sprites->get(entity);
      const Transform& transform = transforms->get(entity);
      const Spawned& spawn = spawned->get(entity);
      ++candidates;
      Pose pose{transform.pos, transform.rot, transform.scale};
      if (transform.teleport_tick != now && prevs->contains(entity)) {
        const PrevTransform& prev = prevs->get(entity);
        if (prev.tick + 1 == now) {
          pose.pos = pose.pos * a + prev.pos * b;
          pose.scale = pose.scale * a + prev.scale * b;
          pose.rot -= ShortestArc(prev.rot, pose.rot) * b;
          ++interpolated;
        }
      }
      const Mark mark{sprite.tint, sprite.flip_x, sprite.flip_y,
                      spawn.spawn_serial};
      if (EmitQuad(pose, sprite, mark, canvas, range.begin + visible,
                   out[visible], items[visible])) {
        bits.Add(items[visible]);
        ++visible;
      }
    }
    RenderListScratch::Chunk& result = scratch.chunks[range.chunk];
    result.begin = range.begin;
    result.visible = visible;
    result.candidates = candidates;
    result.interpolated = interpolated;
    result.lo_or = bits.lo_or;
    result.lo_and = bits.lo_and;
    result.hi_or = bits.hi_or;
    result.hi_and = bits.hi_and;
  });

  // The visible items in chunk order; each still names its quad's slot.
  KeyBits bits;
  std::uint32_t visible = 0;
  std::uint32_t interpolated = 0;
  std::uint32_t candidates = 0;
  SortItem* items = scratch.items.data();
  for (const RenderListScratch::Chunk& chunk : scratch.chunks) {
    if (chunk.begin != visible) {
      std::copy(items + chunk.begin, items + chunk.begin + chunk.visible,
                items + visible);
    }
    visible += chunk.visible;
    candidates += chunk.candidates;
    interpolated += chunk.interpolated;
    bits.lo_or |= chunk.lo_or;
    bits.lo_and &= chunk.lo_and;
    bits.hi_or |= chunk.hi_or;
    bits.hi_and &= chunk.hi_and;
  }
  FinishList(list, visible, bits, candidates, interpolated);
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
