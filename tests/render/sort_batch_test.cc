// Copyright 2026 The simple-engine-2 Authors.
//
// BuildRenderList ordering and batching: (layer, texture, z, spawn serial),
// independent of EnTT's storage order, and batches cut where the texture
// changes. Each sprite's tint carries its test id, so the quads can be
// matched back to the sprites.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <tuple>
#include <vector>

#include "render/render_fixture.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
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

Rgba8 IdTint(std::uint32_t id) {
  return {static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(id >> 8),
          static_cast<std::uint8_t>(id >> 16),
          static_cast<std::uint8_t>(id >> 24)};
}

std::uint32_t TintId(Rgba8 tint) {
  return static_cast<std::uint32_t>(tint.r) |
         (static_cast<std::uint32_t>(tint.g) << 8) |
         (static_cast<std::uint32_t>(tint.b) << 16) |
         (static_cast<std::uint32_t>(tint.a) << 24);
}

struct Key {
  std::int16_t layer;
  std::uint32_t texture;
  float z;
};

// Adds a 2 x 2 sprite inside a 100 x 100 camera, tinted with its id.
entt::entity Add(World& world, std::uint32_t id, const Key& key)
    SE_REQUIRES(g_sim_owner) {
  SpriteRenderer sprite = testing::CenteredSprite(key.texture, 2.0F, 2.0F);
  sprite.layer = key.layer;
  sprite.z = key.z;
  sprite.tint = IdTint(id);
  return testing::AddSprite(world, id, {50.0F, 50.0F}, sprite);
}

std::vector<std::uint32_t> Order(const RenderList& list) {
  std::vector<std::uint32_t> ids;
  ids.reserve(list.quads.size());
  for (const SpriteQuad& quad : list.quads) ids.push_back(TintId(quad.tint));
  return ids;
}

std::vector<std::uint32_t> BuildOrder(const World& world) {
  RenderList list;
  BuildRenderList(world, testing::WorldCamera(100.0F, 100.0F), 1.0F, list);
  return Order(list);
}

TEST_CASE("Quads sort by layer, texture, z, then spawn serial",
          "[render][sort]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  // Created (spawn serial) in id order 1..10.
  static_cast<void>(Add(world, 1, {1, 5, 0.0F}));
  static_cast<void>(Add(world, 2, {0, 9, 0.0F}));
  static_cast<void>(Add(world, 3, {0, 5, 2.0F}));
  static_cast<void>(Add(world, 4, {0, 5, -1.0F}));
  static_cast<void>(Add(world, 5, {-1, 9, 0.0F}));
  static_cast<void>(Add(world, 6, {0, 5, 0.0F}));
  static_cast<void>(Add(world, 7, {0, 5, -0.0F}));  // ties with +0: serial
  static_cast<void>(Add(world, 8, {0, 5, 0.0F}));
  static_cast<void>(Add(world, 9, {-32768, 0xFFFFFFFFU, 1e30F}));
  static_cast<void>(Add(world, 10, {32767, 0, -1e30F}));
  const std::vector<std::uint32_t> expected = {9, 5, 4, 6, 7, 8, 3, 2, 1, 10};
  CHECK(BuildOrder(world) == expected);
}

TEST_CASE("The order does not depend on EnTT's storage order",
          "[render][sort]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  std::vector<entt::entity> entities;
  for (std::uint32_t id = 1; id <= 40; ++id) {
    // Fifteen (texture, z) groups of equal keys: serial breaks the ties.
    entities.push_back(
        Add(world, id, {0, id % 3, static_cast<float>(id % 5) * 0.5F}));
  }
  const std::vector<std::uint32_t> before = BuildOrder(world);
  REQUIRE(before.size() == 40);

  SECTION("reversed packed arrays") {
    world.Reg().sort<SpriteRenderer>(
        [](entt::entity a, entt::entity b) { return a > b; });
    world.Reg().sort<Transform>(
        [](entt::entity a, entt::entity b) { return a > b; });
    CHECK(BuildOrder(world) == before);
  }
  SECTION("components removed and added back") {
    for (std::size_t i = 0; i < entities.size(); i += 3) {
      const entt::entity e = entities[i];
      const SpriteRenderer sprite = world.CReg().get<SpriteRenderer>(e);
      world.Reg().remove<SpriteRenderer>(e);
      world.Reg().emplace<SpriteRenderer>(e, sprite);
    }
    CHECK(BuildOrder(world) == before);
  }
}

TEST_CASE("The order matches a reference sort over random keys",
          "[render][sort]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  for (std::uint64_t seed = 1; seed <= 4; ++seed) {
    INFO("seed " << seed);
    World world(testing::RenderRegistry(), {});
    struct Expected {
      std::int16_t layer;
      std::uint32_t texture;
      float z;
      std::uint32_t serial;
      std::uint32_t id;
    };
    std::vector<Expected> expected;
    const float zs[] = {-0.0F, 0.0F, 1.0F, -1.0F, 0.5F, -1e-38F, 3e38F};
    const std::uint32_t textures[] = {0, 1, 2, 0x10000U, 0xFFFFFFFFU};
    for (std::uint32_t id = 1; id <= 3000; ++id) {
      rng::Stream draws(seed, 0, Guid{seed, id}, 0);
      Key key{};
      key.layer =
          static_cast<std::int16_t>(static_cast<int>(draws.Below(7)) - 3 +
                                    (draws.Below(50) == 0 ? 30000 : 0));
      key.texture = textures[draws.Below(5)];
      key.z = zs[draws.Below(7)];
      const entt::entity e = Add(world, id, key);
      expected.push_back({key.layer, key.texture, key.z == 0.0F ? 0.0F : key.z,
                          world.CReg().get<Spawned>(e).spawn_serial, id});
    }
    std::sort(expected.begin(), expected.end(),
              [](const Expected& a, const Expected& b) {
                return std::tie(a.layer, a.texture, a.z, a.serial) <
                       std::tie(b.layer, b.texture, b.z, b.serial);
              });
    std::vector<std::uint32_t> ids;
    ids.reserve(expected.size());
    for (const Expected& e : expected) ids.push_back(e.id);
    CHECK(BuildOrder(world) == ids);
  }
}

TEST_CASE("Batches are runs of one texture in draw order", "[render][batch]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  RenderList list;
  const Camera2D camera = testing::WorldCamera(100.0F, 100.0F);

  SECTION("an empty World") {
    BuildRenderList(world, camera, 1.0F, list);
    CHECK(list.quads.empty());
    CHECK(list.batches.empty());
  }
  SECTION("textures split by layers") {
    // Layer 0: textures 4, 4, 9; layer 1: 4; layer 2: 4, 4.
    static_cast<void>(Add(world, 1, {0, 4, 0.0F}));
    static_cast<void>(Add(world, 2, {0, 9, 0.0F}));
    static_cast<void>(Add(world, 3, {1, 4, 0.0F}));
    static_cast<void>(Add(world, 4, {0, 4, 1.0F}));
    static_cast<void>(Add(world, 5, {2, 4, 0.0F}));
    static_cast<void>(Add(world, 6, {2, 4, 0.0F}));
    BuildRenderList(world, camera, 1.0F, list);
    REQUIRE(list.batches.size() == 3);
    REQUIRE(list.batches[0].texture == 4);
    REQUIRE(list.batches[0].first == 0);
    REQUIRE(list.batches[0].count == 2);
    REQUIRE(list.batches[1].texture == 9);
    REQUIRE(list.batches[1].first == 2);
    REQUIRE(list.batches[1].count == 1);
    // A layer change with the same texture does not cut the batch.
    REQUIRE(list.batches[2].texture == 4);
    REQUIRE(list.batches[2].first == 3);
    REQUIRE(list.batches[2].count == 3);
    const std::vector<std::uint32_t> order = {1, 4, 2, 3, 5, 6};
    CHECK(Order(list) == order);
  }
  SECTION("one texture is one batch") {
    for (std::uint32_t id = 1; id <= 100; ++id) {
      static_cast<void>(
          Add(world, id,
              {static_cast<std::int16_t>(id % 4), 3, static_cast<float>(id)}));
    }
    BuildRenderList(world, camera, 1.0F, list);
    REQUIRE(list.batches.size() == 1);
    REQUIRE(list.batches[0].first == 0);
    REQUIRE(list.batches[0].count == 100);
  }
  SECTION("culled sprites do not split a batch") {
    // Drawn, the middle sprite would give the batches 4, 9, 4.
    static_cast<void>(Add(world, 1, {0, 4, 0.0F}));
    const entt::entity hidden = Add(world, 2, {1, 9, 0.0F});
    testing::MutableTransform(world, hidden).pos = {-100.0F, 0.0F};
    static_cast<void>(Add(world, 3, {2, 4, 0.0F}));
    BuildRenderList(world, camera, 1.0F, list);
    REQUIRE(list.batches.size() == 1);
    REQUIRE(list.batches[0].count == 2);
  }
}

}  // namespace
}  // namespace se
