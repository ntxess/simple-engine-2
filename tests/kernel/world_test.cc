// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/world.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "support/test_roles.h"

namespace se {
namespace {

const ComponentRegistry& CoreRegistry() {
  static const ComponentRegistry kRegistry = [] {
    ComponentRegistry r;
    RegisterCoreComponents(r);
    r.Seal();
    return r;
  }();
  return kRegistry;
}

std::size_t StorageCount(const World& world) {
  std::size_t count = 0;
  for ([[maybe_unused]] const auto& entry : world.CReg().storage()) ++count;
  return count;
}

TEST_CASE("ENTT_USE_ATOMIC is defined everywhere", "[world]") {
#ifdef ENTT_USE_ATOMIC
  SUCCEED("ENTT_USE_ATOMIC is defined");
#else
  FAIL("ENTT_USE_ATOMIC must be defined (se::entt sets it)");
#endif
}

TEST_CASE("The constructor pre-creates every registered storage", "[world]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {.name = "test"});
  for (const ComponentInfo& info : CoreRegistry().All()) {
    INFO(info.name);
    CHECK(world.CReg().storage(info.id) != nullptr);
  }
  const std::size_t before = StorageCount(world);
  // Using the World never adds a storage.
  const entt::entity e = world.Create(Guid{1, 1});
  world.Reg().emplace<Transform>(e);
  world.Reg().emplace<Name>(e, "one");
  world.RequestDestroy(e);
  world.FinalizeSpawns();
  world.DrainDestroyRequests();
  static_cast<void>(world.Hash());
  CHECK(StorageCount(world) == before);
}

TEST_CASE("Create, Find and Spawn", "[world]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  const entt::entity a = world.Create(Guid{0, 10});
  REQUIRE(world.Find(Guid{0, 10}) == a);
  CHECK_FALSE(world.Find(Guid{0, 11}).has_value());
  CHECK(world.CReg().get<IdComponent>(a).guid == Guid{0, 10});
  CHECK(world.CReg().get<Spawned>(a).spawn_tick == 0);
  CHECK(world.CReg().get<Spawned>(a).spawn_serial == 0);

  world.MutableClock().Advance();
  const entt::entity b = world.Spawn(Guid{0, 10}, 3);
  const Guid derived = World::DeriveGuid(Guid{0, 10}, 1, 3);
  CHECK(world.Find(derived) == b);
  CHECK(world.CReg().get<Spawned>(b).spawn_tick == 1);
  CHECK(world.CReg().get<Spawned>(b).spawn_serial == 1);
  REQUIRE(world.EntityCount() == 2);
}

TEST_CASE("Destroys are requested, then drained", "[world]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  const entt::entity a = world.Create(Guid{0, 1});
  const entt::entity b = world.Create(Guid{0, 2});
  world.RequestDestroy(a);
  world.RequestDestroy(a);  // twice is fine
  CHECK(world.CReg().valid(a));
  world.DrainDestroyRequests();
  CHECK_FALSE(world.CReg().valid(a));
  CHECK(world.CReg().valid(b));
  CHECK_FALSE(world.Find(Guid{0, 1}).has_value());
  REQUIRE(world.EntityCount() == 1);
  // The Guid can be used again.
  static_cast<void>(world.Create(Guid{0, 1}));
  REQUIRE(world.EntityCount() == 2);
}

TEST_CASE("FinalizeSpawns sets prev = current and the teleport tick",
          "[world]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  world.MutableClock().tick = 7;
  const entt::entity e = world.Create(Guid{0, 1});
  world.Reg().emplace<Transform>(e, Vec2{3.0F, 4.0F}, 0.5F, Vec2{2.0F, 2.0F},
                                 Tick{0});
  const entt::entity bare = world.Create(Guid{0, 2});
  world.FinalizeSpawns();
  const auto& t = world.CReg().get<Transform>(e);
  const auto& p = world.CReg().get<PrevTransform>(e);
  REQUIRE(t.teleport_tick == 7);
  CHECK(p.pos == t.pos);
  CHECK(p.rot == t.rot);
  CHECK(p.scale == t.scale);
  REQUIRE(p.tick == 7);
  CHECK_FALSE(world.CReg().all_of<PrevTransform>(bare));
  // Only entities created since the last call are finalized.
  world.Reg().get<Transform>(e).pos = Vec2{9.0F, 9.0F};
  world.FinalizeSpawns();
  CHECK(world.CReg().get<PrevTransform>(e).pos == Vec2{3.0F, 4.0F});
}

void Populate(World& world, bool reverse) SE_REQUIRES(g_sim_owner) {
  for (int i = 0; i < 20; ++i) {
    const int k = reverse ? 19 - i : i;
    const entt::entity e =
        world.Create(Guid{0, static_cast<std::uint64_t>(k + 1)});
    world.Reg().emplace<Transform>(e, Vec2{static_cast<float>(k), 1.0F}, 0.0F,
                                   Vec2{1.0F, 1.0F}, Tick{0});
    world.Reg().get<Spawned>(e).spawn_serial = static_cast<std::uint32_t>(k);
    if (k % 3 == 0) world.Reg().emplace<Name>(e, "third");
  }
}

TEST_CASE("The world hash is independent of storage order", "[world]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World forward(CoreRegistry(), {});
  World backward(CoreRegistry(), {});
  Populate(forward, false);
  Populate(backward, true);
  CHECK(forward.Hash() == backward.Hash());
  CHECK(forward.Digests() == backward.Digests());

  // Hashed component changes show; -0 and +0 do not differ.
  const WorldHash before = forward.Hash();
  const entt::entity e = *forward.Find(Guid{0, 5});
  forward.Reg().get<Transform>(e).rot = -0.0F;
  CHECK(forward.Hash() == before);
  forward.Reg().get<Transform>(e).rot = 0.25F;
  CHECK(forward.Hash() != before);
  forward.Reg().get<Transform>(e).rot = 0.0F;
  CHECK(forward.Hash() == before);
  forward.Reg().emplace<Name>(e, "renamed");
  CHECK(forward.Hash() != before);

  // The digests are sorted by Guid.
  const std::vector<EntityDigest> digests = backward.Digests();
  for (std::size_t i = 1; i < digests.size(); ++i) {
    CHECK(digests[i - 1].guid < digests[i].guid);
  }
}

TEST_CASE("Clone copies ids, order, Guids and clock", "[world]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {.name = "original", .seed = 9});
  Populate(world, false);
  world.MutableClock().tick = 42;
  // Leave holes in the entity storage.
  world.RequestDestroy(*world.Find(Guid{0, 4}));
  world.RequestDestroy(*world.Find(Guid{0, 9}));
  world.DrainDestroyRequests();

  const std::unique_ptr<World> copy = world.Clone();
  CHECK(copy->Hash() == world.Hash());
  CHECK(copy->Digests() == world.Digests());
  REQUIRE(copy->Clock().tick == 42);
  REQUIRE(copy->Desc().seed == 9);
  for (const entt::entity e : world.CReg().view<const IdComponent>()) {
    const Guid guid = world.CReg().get<IdComponent>(e).guid;
    CHECK(copy->Find(guid) == e);
  }
  // The same creates hand out the same entity ids.
  const entt::entity next_original = world.Create(Guid{1, 1});
  const entt::entity next_copy = copy->Create(Guid{1, 1});
  CHECK(next_original == next_copy);
  const entt::entity third_original = world.Create(Guid{1, 2});
  const entt::entity third_copy = copy->Create(Guid{1, 2});
  CHECK(third_original == third_copy);
  CHECK(copy->Hash() == world.Hash());
}

TEST_CASE("Play Worlds take mutations through a MutationGate", "[world]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World play(CoreRegistry(), {.kind = WorldKind::kPlay});
  CHECK_FALSE(play.GateOpen());
  {
    MutationGate gate = play.OpenGate(GateReason::kTest);
    CHECK(play.GateOpen());
    CHECK(gate.Reason() == GateReason::kTest);
    static_cast<void>(play.Create(Guid{0, 1}));
    const MutationGate moved = std::move(gate);
    CHECK(play.GateOpen());
  }
  CHECK_FALSE(play.GateOpen());
  REQUIRE(play.EntityCount() == 1);
}

TEST_CASE("The owner is the constructing thread", "[world]") {
  World world(CoreRegistry(), {});
  CHECK(world.Owner() == std::this_thread::get_id());
  std::thread::id other;
  std::thread t([&] { other = std::this_thread::get_id(); });
  t.join();
  world.SetOwner(other);
  CHECK(world.Owner() == other);
  world.SetOwner(std::this_thread::get_id());
}

TEST_CASE("EntityRef resolves by Guid and revalidates its cache", "[world]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  const entt::entity target = world.Create(Guid{0, 77});
  const EntityRef ref{Guid{0, 77}};
  CHECK(ref.Resolve(world) == target);
  CHECK(ref.cached == target);
  world.RequestDestroy(target);
  world.DrainDestroyRequests();
  CHECK(ref.Resolve(world) == entt::entity{entt::null});
  // A new entity with the same Guid resolves, even if it reuses the id.
  const entt::entity again = world.Create(Guid{0, 77});
  CHECK(ref.Resolve(world) == again);
  const EntityRef missing{Guid{5, 5}};
  CHECK(missing.Resolve(world) == entt::entity{entt::null});
}

TEST_CASE("Mutations mark the hash dirty until recorded", "[world]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  CHECK(world.HashDirty());
  static_cast<void>(world.Create(Guid{0, 1}));
  world.RecordHash();
  CHECK_FALSE(world.HashDirty());
  CHECK(world.LastHash() == world.Hash());
  static_cast<void>(world.Reg());
  CHECK(world.HashDirty());
}

}  // namespace
}  // namespace se
