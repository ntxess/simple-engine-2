// Copyright 2026 The simple-engine-2 Authors.
//
// The stress generator's plan: the ported enemy archetype as component JSON
// every hook reads, the explicit distributions, stable Guids and the
// spreading of a press across ticks.

#include "se_editor_panels/stress_generator.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <set>
#include <vector>

#include "editor_panels/support/panel_world.h"
#include "game_rampage/components.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

TEST_CASE("A press makes the same paths for the same seed and press",
          "[editor][stress]") {
  const StressSpec spec = DefaultStressSpec();
  const auto a = StressPaths(spec, 1, 0, 50);
  REQUIRE(a == StressPaths(spec, 1, 0, 50));
  REQUIRE(a != StressPaths(spec, 1, 1, 50));
  REQUIRE(a != StressPaths(spec, 2, 0, 50));
  // A longer press starts with the shorter one's paths.
  const auto longer = StressPaths(spec, 1, 0, 80);
  REQUIRE(std::vector(longer.begin(), longer.begin() + 50) == a);
}

TEST_CASE("Paths have 1 to 10 whole-pixel points inside the arena",
          "[editor][stress]") {
  const StressSpec spec = DefaultStressSpec(1920.0F, 1080.0F);
  const auto paths = StressPaths(spec, 7, 3, 4000);
  std::vector<int> by_count(11, 0);
  float max_x = 0.0F;
  float max_y = 0.0F;
  for (const auto& path : paths) {
    REQUIRE(!path.empty());
    REQUIRE(path.size() <= 10);
    ++by_count[path.size()];
    for (const Vec2 p : path) {
      REQUIRE(p.x >= 0.0F);
      REQUIRE(p.x < 1920.0F);
      REQUIRE(p.y >= 0.0F);
      REQUIRE(p.y < 1080.0F);
      REQUIRE(p.x == static_cast<float>(static_cast<int>(p.x)));
      REQUIRE(p.y == static_cast<float>(static_cast<int>(p.y)));
      max_x = std::max(max_x, p.x);
      max_y = std::max(max_y, p.y);
    }
  }
  // Uniform counts: each of 1..10 near 400 of 4000.
  for (int n = 1; n <= 10; ++n) {
    const bool near_400 = by_count[n] > 300 && by_count[n] < 500;
    CHECK(near_400);
  }
  // Uniform positions reach the far edges (y is not folded at 840).
  CHECK(max_x > 1900.0F);
  CHECK(max_y > 1060.0F);
}

TEST_CASE("Stress Guids are stable and distinct", "[editor][stress]") {
  REQUIRE(StressGuid(1, 0, 0) == StressGuid(1, 0, 0));
  std::set<Guid> seen;
  for (std::uint32_t press = 0; press < 20; ++press) {
    for (std::uint32_t i = 0; i < 200; ++i) {
      const Guid guid = StressGuid(1, press, i);
      REQUIRE_FALSE(guid.IsNil());
      REQUIRE(seen.insert(guid).second);
    }
  }
}

TEST_CASE("A spawn is the enemy archetype at its path's first point",
          "[editor][stress]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::PanelWorld w;
  const StressSpec spec = DefaultStressSpec();
  const std::vector<StressSpawn> spawns = PlanStress(spec, 11, 2, 5);
  REQUIRE(spawns.size() == 5);
  REQUIRE(spawns[0].guid == StressGuid(11, 2, 0));
  REQUIRE(spawns[0].points == StressPaths(spec, 11, 2, 5)[0]);
  REQUIRE(spawns[0].components.front().component == "Transform");

  // Every component reads through the registry's own JSON hooks.
  const ComponentRegistry& registry = w.Registry();
  for (const StressSpawn& spawn : spawns) {
    const MutationGate gate = w.Get().OpenGate(GateReason::kTest);
    const entt::entity e = w.Get().Create(spawn.guid);
    for (const SpawnComponent& c : spawn.components) {
      const ComponentInfo* info = registry.FindByName(c.component);
      REQUIRE(info != nullptr);
      REQUIRE(info->read_json(w.Get().Reg(), e, c.json).has_value());
    }
  }
  const entt::registry& reg = w.CGet().CReg();
  const entt::entity e = *w.CGet().Find(spawns[1].guid);
  REQUIRE(reg.get<Transform>(e).pos == spawns[1].points.front());
  const auto& path = reg.get<rampage::Path>(e);
  REQUIRE(path.points == spawns[1].points);
  REQUIRE(path.loop);
  REQUIRE(reg.get<rampage::Team>(e).id == rampage::TeamId::kEnemy);
  const auto& stats = reg.get<rampage::Stats>(e);
  REQUIRE(*rampage::FindStat(stats, rampage::kStatHp) == 100.0F);
  REQUIRE(*rampage::FindStat(stats, rampage::kStatSpeed) == 500.0F);
  const auto& sprite = reg.get<SpriteRenderer>(e);
  REQUIRE(sprite.texture == TextureKeyOf("player"));
  REQUIRE(sprite.region == Rect{0.0F, 0.0F, 48.0F, 48.0F});
  REQUIRE(sprite.origin == Vec2{24.0F, 24.0F});
  REQUIRE(reg.get<rampage::Collider>(e).half_extents == Vec2{24.0F, 24.0F});
  REQUIRE(reg.all_of<rampage::PathCache, rampage::ActiveEffects>(e));
  REQUIRE_FALSE(reg.all_of<rampage::EffectSource>(e));

  // The spawned enemies walk their paths from the first tick.
  testing::PanelWorld play(WorldKind::kPlay);
  {
    const MutationGate gate = play.Get().OpenGate(GateReason::kTest);
    const entt::entity p = play.Get().Create(spawns[1].guid);
    for (const SpawnComponent& c : spawns[1].components) {
      REQUIRE(play.Registry()
                  .FindByName(c.component)
                  ->read_json(play.Get().Reg(), p, c.json)
                  .has_value());
    }
  }
  play.Finish();
  play.Step(30);
  const entt::entity p = *play.CGet().Find(spawns[1].guid);
  if (spawns[1].points.size() > 1 &&
      spawns[1].points[1] != spawns[1].points[0]) {
    CHECK(play.CGet().CReg().get<Transform>(p).pos != spawns[1].points[0]);
  }
}

TEST_CASE("A press is spread over ticks in batches", "[editor][stress]") {
  REQUIRE(SpreadSpawns(0, 64).empty());
  REQUIRE(SpreadSpawns(10, 64) == std::vector<SpawnBatch>{{0, 10, 0}});
  REQUIRE(SpreadSpawns(130, 64) ==
          std::vector<SpawnBatch>{{0, 64, 0}, {64, 64, 1}, {128, 2, 2}});
  REQUIRE(SpreadSpawns(3, 0) ==
          std::vector<SpawnBatch>{{0, 1, 0}, {1, 1, 1}, {2, 1, 2}});
}

}  // namespace
}  // namespace se::editor
