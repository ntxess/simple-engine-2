// Copyright 2026 The simple-engine-2 Authors.
//
// renderlist_10k (docs/milestone-specs.md M3c, the M3 exit gate): building
// the RenderList of n sprites (10k by default) over 8 textures with seeded
// random poses, in Free cadence, so every sprite is interpolated. The list
// is warm: it has built this World before the timed iterations, so the
// iterations measure interpolation, culling, sorting and batching without
// allocation.
//
// Scene: sprites spread over a 1920 x 1080 arena plus a 10% margin on each
// side (so a few percent are culled), on 4 layers with random z, 32 x 32
// atlas cells of 8 textures, scales 0.5 to 2, any rotation, random flips,
// and one tick of motion between PrevTransform and Transform.

#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>

#include "se_bench/suite.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/rng.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"

namespace se::bench {
namespace {

constexpr float kArenaW = 1920.0F;
constexpr float kArenaH = 1080.0F;
constexpr std::uint32_t kTextures = 8;
constexpr std::uint64_t kGuidHi = 0x5EB0000000000000ULL;

std::expected<void, Error> RunRenderList(const SuiteOptions& options,
                                         Sampler& sampler) {
  AssertMainThread();
  const InlineOwnerScope owner(OnMainThread());
  ComponentRegistry registry;
  RegisterCoreComponents(registry);
  RegisterRenderComponents(registry);
  registry.Seal();
  World world(registry, {.name = "renderlist_10k", .seed = options.seed});

  for (std::uint32_t i = 0; i < options.n; ++i) {
    rng::Stream draws(options.seed, 0, Guid{kGuidHi, i}, 0);
    const entt::entity entity = world.Create(Guid{kGuidHi, i + 1});
    Transform transform;
    transform.pos = {kArenaW * (1.2F * draws.Unit() - 0.1F),
                     kArenaH * (1.2F * draws.Unit() - 0.1F)};
    transform.rot = 360.0F * draws.Unit();
    const float scale = 0.5F + 1.5F * draws.Unit();
    transform.scale = {scale, scale};
    world.Reg().emplace<Transform>(entity, transform);

    SpriteRenderer sprite;
    sprite.texture = 1 + draws.Below(kTextures);
    sprite.region = {32.0F * static_cast<float>(draws.Below(16)),
                     32.0F * static_cast<float>(draws.Below(16)), 32.0F, 32.0F};
    sprite.origin = {16.0F, 16.0F};
    const auto tint = static_cast<std::uint32_t>(draws.Next());
    sprite.tint = {static_cast<std::uint8_t>(tint),
                   static_cast<std::uint8_t>(tint >> 8),
                   static_cast<std::uint8_t>(tint >> 16), 255};
    sprite.layer = static_cast<std::int16_t>(draws.Below(4));
    sprite.z = draws.Unit();
    sprite.flip_x = draws.Below(2) == 1;
    sprite.flip_y = draws.Below(8) == 1;
    world.Reg().emplace<SpriteRenderer>(entity, sprite);
  }
  world.FinalizeSpawns();

  // One tick of motion: PrevTransform is the pose before it, as after the
  // Schedule's PrevTransformCopy, so every sprite interpolates.
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
    transform.rot += 20.0F * draws.Unit() - 10.0F;
  }

  Camera2D camera;
  camera.center = {0.5F * kArenaW, 0.5F * kArenaH};
  camera.virtual_size = {kArenaW, kArenaH};
  camera.primary = true;

  RenderList list;
  BuildRenderList(world, camera, 0.5F, list);
  std::uint32_t frame = 0;
  // Free-cadence alphas as a 144 Hz display sees 120 Hz ticks: each frame
  // advances alpha by 5/6 of a tick, modulo 1.
  sampler.Run([&] {
    const float alpha = static_cast<float>((5 * frame) % 6) / 6.0F;
    ++frame;
    BuildRenderList(world, camera, alpha, list);
  });

  sampler.AddCounter("sprites", list.stats.sprites);
  sampler.AddCounter("interpolated", list.stats.interpolated);
  sampler.AddCounter("culled", list.stats.culled);
  sampler.AddCounter("quads", list.stats.quads);
  sampler.AddCounter("batches", list.stats.batches);
  sampler.AddCounter("textures", kTextures);
  return {};
}

const SuiteRegistrar kRenderList10k({
    .name = "renderlist_10k",
    .description = "BuildRenderList of 10k interpolated sprites, 8 textures",
    .default_n = 10000,
    .default_iterations = 1000,
    .default_warmup = 50,
    .run = &RunRenderList,
});

}  // namespace
}  // namespace se::bench
