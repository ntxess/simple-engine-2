// Copyright 2026 The simple-engine-2 Authors.
//
// render_frame_10k (docs/milestone-specs.md M7, "Bench"; the M7 exit gate):
// the CPU side of a Renderer2D frame for n sprites (10k by default), all
// visible: BuildRenderList, then the sprite vertices (WriteSpriteVertices)
// into a pre-sized buffer, as the renderer's build phase does before its
// copy pass. p95 at most 1 ms in Release.
//
// Scene: as renderlist_10k (8 textures of 32 x 32 atlas cells, 4 layers,
// random z, scales 0.5 to 2, any rotation, random flips, one tick of
// motion, Free-cadence alphas), but every sprite lies inside the 1920 x
// 1080 canvas with a margin of its largest half-diagonal, so nothing is
// culled and all n quads get vertices.

#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <vector>

#include "se_bench/suite.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/detmath.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/rng.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"
#include "se_render_core/sprite_vertices.h"

namespace se::bench {
namespace {

constexpr float kArenaW = 1920.0F;
constexpr float kArenaH = 1080.0F;
// Half the diagonal of a 32 px cell at scale 2, plus 4 px of motion.
constexpr float kMargin = 50.0F;
constexpr std::uint32_t kTextures = 8;
constexpr float kTurn = static_cast<float>(2.0 * dm::kPi);
constexpr std::uint64_t kGuidHi = 0x5EB1000000000000ULL;

std::expected<void, Error> RunRenderFrame(const SuiteOptions& options,
                                          Sampler& sampler) {
  AssertMainThread();
  const InlineOwnerScope owner(OnMainThread());
  ComponentRegistry registry;
  RegisterCoreComponents(registry);
  RegisterRenderComponents(registry);
  registry.Seal();
  World world(registry, {.name = "render_frame_10k", .seed = options.seed});

  for (std::uint32_t i = 0; i < options.n; ++i) {
    rng::Stream draws(options.seed, 0, Guid{kGuidHi, i}, 0);
    const entt::entity entity = world.Create(Guid{kGuidHi, i + 1});
    Transform transform;
    transform.pos = {kMargin + (kArenaW - 2.0F * kMargin) * draws.Unit(),
                     kMargin + (kArenaH - 2.0F * kMargin) * draws.Unit()};
    transform.rot = kTurn * draws.Unit();
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
    transform.rot += kTurn * (draws.Unit() - 0.5F) / 18.0F;
  }

  Camera2D camera;
  camera.center = {0.5F * kArenaW, 0.5F * kArenaH};
  camera.virtual_size = {kArenaW, kArenaH};
  camera.primary = true;

  // Warm: the list and the vertex buffer hold n sprites before timing.
  RenderList list;
  std::vector<SpriteVertex> vertices(std::size_t{options.n} * kVerticesPerQuad);
  BuildRenderList(world, camera, 0.5F, list);
  WriteSpriteVertices(list.quads, vertices);
  std::uint32_t frame = 0;
  sampler.Run([&] {
    const float alpha = static_cast<float>((5 * frame) % 6) / 6.0F;
    ++frame;
    BuildRenderList(world, camera, alpha, list);
    WriteSpriteVertices(list.quads, vertices);
  });

  sampler.AddCounter("sprites", list.stats.sprites);
  sampler.AddCounter("culled", list.stats.culled);
  sampler.AddCounter("quads", list.stats.quads);
  sampler.AddCounter("batches", list.stats.batches);
  sampler.AddCounter("vertex_bytes", static_cast<double>(list.stats.quads) *
                                         kVerticesPerQuad *
                                         sizeof(SpriteVertex));
  if (list.stats.culled != 0) {
    return Fail(ErrorCode::kInvalidArgument,
                "render_frame_10k: sprites were culled; the scene must keep "
                "every sprite visible");
  }
  return {};
}

const SuiteRegistrar kRenderFrame10k({
    .name = "render_frame_10k",
    .description = "RenderList and sprite vertices of 10k visible sprites",
    .default_n = 10000,
    .default_iterations = 1000,
    .default_warmup = 50,
    .run = &RunRenderFrame,
});

}  // namespace
}  // namespace se::bench
