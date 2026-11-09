// Copyright 2026 The simple-engine-2 Authors.
//
// A warm RenderList allocates nothing, and building one leaves the World
// untouched (rendering never writes simulation state).

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <type_traits>
#include <vector>

#include "render/alloc_counter.h"
#include "render/render_fixture.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/rng.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"
#include "support/test_roles.h"

namespace se {
namespace {

// The builder sees the World only as const.
static_assert(std::is_same_v<decltype(&BuildRenderList),
                             void (*)(const World&, const Camera2D&, float,
                                      RenderList&)>);

struct Buffer {
  const void* data = nullptr;
  std::size_t capacity = 0;

  friend bool operator==(const Buffer&, const Buffer&) = default;
};

template <class T>
Buffer Of(const std::vector<T>& v) {
  return {v.data(), v.capacity()};
}

std::array<Buffer, 5> Buffers(const RenderList& list) {
  return {Of(list.quads), Of(list.batches), Of(list.scratch.quads),
          Of(list.scratch.items), Of(list.scratch.items_swap)};
}

// |count| sprites with seeded poses over 8 textures and 4 layers, about a
// quarter of them outside a 1920 x 1080 camera.
std::vector<entt::entity> Populate(World& world, std::uint32_t count)
    SE_REQUIRES(g_sim_owner) {
  std::vector<entt::entity> entities;
  for (std::uint32_t i = 0; i < count; ++i) {
    rng::Stream draws(7, 0, Guid{1, i}, 0);
    SpriteRenderer sprite =
        testing::CenteredSprite(draws.Below(8), 8.0F + 24.0F * draws.Unit(),
                                8.0F + 24.0F * draws.Unit());
    sprite.layer = static_cast<std::int16_t>(draws.Below(4));
    sprite.z = draws.Unit();
    const Vec2 pos{-240.0F + 2400.0F * draws.Unit(),
                   -135.0F + 1350.0F * draws.Unit()};
    entities.push_back(testing::AddSprite(
        world, i + 1, pos, sprite, testing::Degrees(360.0F) * draws.Unit()));
  }
  return entities;
}

// Moves every sprite as a tick of movement would.
void Move(World& world, const std::vector<entt::entity>& entities, float step)
    SE_REQUIRES(g_sim_owner) {
  testing::StepTick(world);
  for (const entt::entity e : entities) {
    Transform& t = testing::MutableTransform(world, e);
    t.pos.x += step;
    t.rot += testing::Degrees(3.0F);
  }
}

TEST_CASE("A warm RenderList allocates nothing", "[render][alloc]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  const std::vector<entt::entity> entities = Populate(world, 4000);
  Camera2D camera = testing::WorldCamera(1920.0F, 1080.0F);

  // A cold list allocates: the counter sees this path.
  RenderList list;
  const std::uint64_t cold = testing::AllocationCount();
  BuildRenderList(world, camera, 0.5F, list);
  if (testing::CountingAllocations()) {
    CHECK(testing::AllocationCount() > cold);
  }
  REQUIRE(list.stats.quads > 0);
  REQUIRE(list.stats.culled > 0);
  const std::array<Buffer, 5> warm = Buffers(list);

  std::uint64_t allocated = 0;
  const auto build = [&](float alpha) {
    const std::uint64_t start = testing::AllocationCount();
    BuildRenderList(world, camera, alpha, list);
    allocated += testing::AllocationCount() - start;
  };
  for (int frame = 0; frame < 120; ++frame) {
    if (frame % 2 == 0) Move(world, entities, frame % 4 == 0 ? 7.0F : -5.0F);
    // The camera wanders, so the visible set and the batches change.
    camera.center.x = 960.0F + 4.0F * static_cast<float>(frame % 50);
    camera.zoom = frame % 3 == 0 ? 1.0F : 1.25F;
    build(static_cast<float>(frame % 7) / 6.0F);
  }
  CHECK(Buffers(list) == warm);

  // Fewer sprites, then as many as before again.
  for (std::size_t i = 0; i < entities.size(); i += 2) {
    world.Reg().remove<SpriteRenderer>(entities[i]);
  }
  build(1.0F);
  REQUIRE(list.stats.sprites == 2000);
  for (std::size_t i = 0; i < entities.size(); i += 2) {
    world.Reg().emplace<SpriteRenderer>(
        entities[i], testing::CenteredSprite(1, 16.0F, 16.0F));
  }
  build(1.0F);
  REQUIRE(list.stats.sprites == 4000);
  CHECK(Buffers(list) == warm);
  if (testing::CountingAllocations()) {
    REQUIRE(allocated == 0);
  } else {
    WARN("allocation counting is off in sanitizer builds; buffers checked");
  }
}

TEST_CASE("Building a RenderList leaves the World unchanged",
          "[render][alloc]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  const std::vector<entt::entity> entities = Populate(world, 500);
  Move(world, entities, 3.0F);
  world.RecordHash();
  REQUIRE_FALSE(world.HashDirty());
  const WorldHash recorded = world.LastHash();
  const WorldHash current = world.Hash();
  const Tick tick = world.Clock().tick;

  RenderList list;
  for (const float alpha : {0.0F, 0.5F, 1.0F}) {
    BuildRenderList(world, testing::WorldCamera(1920.0F, 1080.0F), alpha, list);
  }
  CHECK_FALSE(world.HashDirty());
  CHECK(world.LastHash() == recorded);
  CHECK(world.Hash() == current);
  CHECK(world.Clock().tick == tick);
}

}  // namespace
}  // namespace se
