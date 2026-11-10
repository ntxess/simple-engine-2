// Copyright 2026 The simple-engine-2 Authors.
//
// snapshot_publish_10k (docs/milestone-specs.md M17a, Bench): the dedicated
// sim driver's per-tick publish of n sprites (10k by default): acquiring a
// free slot of a SnapshotChannel, filling its render part from the World
// (AppendRenderView: n RenderItems, the sparse index, the view) and
// publishing it. Each iteration also takes the slot on the platform side
// and returns it, as a frame would, so the next iteration finds it free;
// that part is two ring operations. The slot is warm: it has held this
// World before the timed iterations, so they allocate nothing.
//
// Scene: renderlist_10k's (8 textures of 32 x 32 atlas cells, 4 layers,
// seeded poses over a 1920 x 1080 arena plus a 10% margin, one tick of
// motion between PrevTransform and Transform).

#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>

#include "se_bench/suite.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/detmath.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/rng.h"
#include "se_core/snapshot.h"
#include "se_core/snapshot_channel.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_render_core/snapshot_fill.h"

namespace se::bench {
namespace {

constexpr float kArenaW = 1920.0F;
constexpr float kArenaH = 1080.0F;
constexpr std::uint32_t kTextures = 8;
constexpr float kTurn = static_cast<float>(2.0 * dm::kPi);
constexpr std::uint64_t kGuidHi = 0x5EB1000000000000ULL;

std::expected<void, Error> RunSnapshotPublish(const SuiteOptions& options,
                                              Sampler& sampler) {
  AssertMainThread();
  const InlineOwnerScope owner(OnMainThread());
  ComponentRegistry registry;
  RegisterCoreComponents(registry);
  RegisterRenderComponents(registry);
  registry.Seal();
  World world(registry, {.name = "snapshot_publish_10k", .seed = options.seed});

  for (std::uint32_t i = 0; i < options.n; ++i) {
    rng::Stream draws(options.seed, 0, Guid{kGuidHi, i}, 0);
    const entt::entity entity = world.Create(Guid{kGuidHi, i + 1});
    Transform transform;
    transform.pos = {kArenaW * (1.2F * draws.Unit() - 0.1F),
                     kArenaH * (1.2F * draws.Unit() - 0.1F)};
    transform.rot = kTurn * draws.Unit();
    const float scale = 0.5F + 1.5F * draws.Unit();
    transform.scale = {scale, scale};
    world.Reg().emplace<Transform>(entity, transform);

    SpriteRenderer sprite;
    sprite.texture = 1 + draws.Below(kTextures);
    sprite.region = {32.0F * static_cast<float>(draws.Below(16)),
                     32.0F * static_cast<float>(draws.Below(16)), 32.0F, 32.0F};
    sprite.origin = {16.0F, 16.0F};
    sprite.layer = static_cast<std::int16_t>(draws.Below(4));
    sprite.z = draws.Unit();
    sprite.flip_x = draws.Below(2) == 1;
    world.Reg().emplace<SpriteRenderer>(entity, sprite);
  }
  world.FinalizeSpawns();
  // One tick of motion, as after the Schedule's PrevTransformCopy.
  world.MutableClock().Advance();
  const Tick previous = world.Clock().tick - 1;
  for (const auto [entity, transform, prev, id] :
       world.Reg().view<Transform, PrevTransform, const IdComponent>().each()) {
    prev.pos = transform.pos;
    prev.rot = transform.rot;
    prev.scale = transform.scale;
    prev.tick = previous;
    rng::Stream draws(options.seed, 1, id.guid, 1);
    transform.pos = transform.pos + Vec2{8.0F * draws.Unit() - 4.0F,
                                         8.0F * draws.Unit() - 4.0F};
  }

  RenderViewOptions view;
  view.camera.center = {0.5F * kArenaW, 0.5F * kArenaH};
  view.camera.virtual_size = {kArenaW, kArenaH};
  view.camera.primary = true;

  // One slot, so every iteration fills the same warm one.
  SnapshotChannel channel(1);
  std::uint32_t items = 0;
  std::uint64_t dropped = 0;
  const auto publish = [&] {
    Snapshot* slot = channel.AcquireForWrite();
    if (slot == nullptr) {
      ++dropped;
      return;
    }
    slot->ClearRender();
    slot->header.tick = world.Clock().tick;
    slot->header.render = true;
    AppendRenderView(world, view, *slot);
    items = static_cast<std::uint32_t>(slot->items.size());
    channel.Publish(slot);
    // The platform's side: take it and give it back.
    channel.Receive([](SlotLease /*lease*/) {});
  };
  publish();
  sampler.Run(publish);
  if (dropped != 0) {
    return std::unexpected(Error{
        ErrorCode::kInvalidArgument, "a publish found no free slot", {}, 0, 0});
  }

  sampler.AddCounter("items", items);
  sampler.AddCounter("slot_bytes",
                     static_cast<double>(items) * sizeof(RenderItem));
  return {};
}

const SuiteRegistrar kSnapshotPublish10k({
    .name = "snapshot_publish_10k",
    .description = "Fill and publish one snapshot slot of 10k sprites",
    .default_n = 10000,
    .default_iterations = 1000,
    .default_warmup = 50,
    .run = &RunSnapshotPublish,
});

}  // namespace
}  // namespace se::bench
