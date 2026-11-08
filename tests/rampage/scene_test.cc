// Copyright 2026 The simple-engine-2 Authors.
//
// Scenes and prefabs (se-scene-v1): the sandbox scene and the prefabs load;
// merge patches override values and replace arrays whole; saving and
// reloading a World gives the same hash; and malformed scenes are errors.

#include "game_rampage/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/module.h"
#include "rampage/support/rampage_session.h"
#include "rampage/support/test_world.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "support/test_roles.h"

namespace se::rampage {
namespace {

entt::entity ByName(const World& world, std::string_view name) {
  for (const auto [e, n] : world.CReg().view<const Name>().each()) {
    if (n.value == name) return e;
  }
  return entt::null;
}

SceneLoadOptions AssetOptions() {
  SceneLoadOptions options;
  options.asset_root = testing::RampageAssetRoot();
  return options;
}

// Prefabs from memory.
SceneLoadOptions MemoryPrefabs(
    const std::map<std::string, std::string>& prefabs) {
  SceneLoadOptions options;
  const auto shared =
      std::make_shared<const std::map<std::string, std::string>>(prefabs);
  options.resolver =
      [shared](std::string_view ref) -> std::expected<std::string, Error> {
    const auto it = shared->find(std::string(ref));
    if (it == shared->end()) return Fail(ErrorCode::kNotFound, "no prefab");
    return it->second;
  };
  return options;
}

constexpr std::string_view kUnitPrefab = R"({
  "format": "se-prefab-v1",
  "components": {
    "Transform": {"pos": {"x": 10, "y": 20}},
    "Team": {"id": "enemy"},
    "Stats": {"HP": 100, "Speed": 500},
    "EffectSource": {"effects": [
      {"kind": "instant", "stat": "HP", "value": -1},
      {"kind": "temp_timed", "stat": "Armor", "value": 2,
       "duration_ms": 100}]},
    "PlayerControl": {"speed": 90}
  }
})";

std::string Scene(std::string_view entities) {
  return std::string(R"({"format": "se-scene-v1", "entities": [)") +
         std::string(entities) + "]}";
}

TEST_CASE("The sandbox scene and its prefabs load", "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  const auto loaded = LoadSceneFile(
      w.Get(), testing::RampageAssetRoot() + "/scenes/sandbox.scene.json",
      AssetOptions());
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->name == "sandbox");
  REQUIRE(loaded->entities == 17);
  const entt::registry& reg = w.Get().CReg();
  CHECK(reg.ctx().get<ArenaBounds>().w == 1920.0F);
  CHECK(reg.ctx().get<SceneSeed>().value == 1);
  CHECK(reg.ctx().get<EffectSettings>().event_watchdog_ms == 36000);

  const entt::entity player = ByName(w.Get(), "player");
  REQUIRE(player != entt::entity{entt::null});
  CHECK(reg.get<Spawned>(player).spawn_serial == 0);
  CHECK(reg.get<Transform>(player).pos == Vec2{200.0F, 200.0F});
  CHECK(reg.get<Team>(player).id == TeamId::kFriendly);
  CHECK(reg.get<EffectSource>(player).effects.size() == 3);
  CHECK(reg.get<EffectSource>(player).effects[0].stat == kStatHp);
  CHECK(*w.Stat(player, "HP") == 100.0F);
  CHECK(reg.all_of<PairCooldowns, ActiveEffects, PlayerControl, MoveIntent,
                   KinematicBody, Collider>(player));

  const entt::entity enemy = ByName(w.Get(), "enemy 1");
  REQUIRE(enemy != entt::entity{entt::null});
  const Path& path = reg.get<Path>(enemy);
  REQUIRE(path.points.size() == 5);
  CHECK(path.loop);
  // It starts on its first point (the prefab's Transform has no position).
  CHECK(reg.get<Transform>(enemy).pos == path.points.front());
  CHECK(*w.Stat(enemy, "Speed") == 500.0F);
  CHECK(reg.all_of<PathCache, ActiveEffects>(enemy));
  CHECK(reg.ctx().get<StatNames>().Name(kStatSpeed) == "Speed");
}

TEST_CASE("Merge patches override values and replace arrays whole",
          "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  const auto loaded =
      LoadScene(w.Get(), Scene(R"(
    {"guid": "00000000-0000-0001-0000-000000000001", "name": "plain",
     "prefab": "unit"},
    {"guid": "00000000-0000-0001-0000-000000000002", "name": "patched",
     "prefab": "unit",
     "overrides": {"Stats": {"Speed": 250, "Luck": 7},
                   "Transform": {"pos": {"y": 99}},
                   "EffectSource": {"effects": [
                     {"kind": "over_time", "stat": "HP", "value": -3,
                      "duration_ms": 1000, "interval_ms": 500}]},
                   "PlayerControl": null}},
    {"guid": "00000000-0000-0001-0000-000000000003", "name": "replaced",
     "prefab": "unit",
     "components": {"Stats": {"HP": 5}}})"),
                MemoryPrefabs({{"unit", std::string(kUnitPrefab)}}));
  REQUIRE(loaded.has_value());
  const entt::registry& reg = w.Get().CReg();

  const entt::entity plain = ByName(w.Get(), "plain");
  CHECK(*w.Stat(plain, "Speed") == 500.0F);
  CHECK(reg.get<EffectSource>(plain).effects.size() == 2);
  CHECK(reg.get<PlayerControl>(plain).speed == 90.0F);

  const entt::entity patched = ByName(w.Get(), "patched");
  // Objects merge member by member.
  CHECK(*w.Stat(patched, "HP") == 100.0F);
  CHECK(*w.Stat(patched, "Speed") == 250.0F);
  CHECK(*w.Stat(patched, "Luck") == 7.0F);
  CHECK(reg.get<Transform>(patched).pos == Vec2{10.0F, 99.0F});
  // Arrays are replaced whole.
  const auto& effects = reg.get<EffectSource>(patched).effects;
  REQUIRE(effects.size() == 1);
  CHECK(effects[0].kind == EffectKind::kOverTime);
  REQUIRE(effects[0].interval_ms == 500);
  // null removes the component.
  CHECK_FALSE(reg.all_of<PlayerControl>(patched));

  // A component in "components" replaces the prefab's whole.
  const entt::entity replaced = ByName(w.Get(), "replaced");
  CHECK(*w.Stat(replaced, "HP") == 5.0F);
  CHECK(w.Stat(replaced, "Speed") == nullptr);
  CHECK(reg.get<Spawned>(replaced).spawn_serial == 2);
  // Effect stats are named too.
  CHECK(reg.ctx().get<StatNames>().Name(StatIdOf("Armor")) == "Armor");
}

TEST_CASE("Saving and reloading a World gives the same hash",
          "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld original;
  REQUIRE(
      LoadSceneFile(original.Get(),
                    testing::RampageAssetRoot() + "/scenes/sandbox.scene.json",
                    AssetOptions()));
  const auto saved = SaveScene(original.Get(), "sandbox");
  REQUIRE(saved.has_value());
  testing::RampageTestWorld reloaded;
  const auto again = LoadScene(reloaded.Get(), *saved, SceneLoadOptions{});
  REQUIRE(again.has_value());
  REQUIRE(again->entities == 17);
  CHECK(reloaded.Get().Hash() == original.Get().Hash());
  CHECK(reloaded.Get().Digests() == original.Get().Digests());
  CHECK(SaveScene(reloaded.Get(), "sandbox").value() == *saved);
  // Stats are saved by name.
  CHECK(saved->find("\"Speed\": 500") != std::string::npos);

  // Mid-game state (effects, cooldowns, path progress) survives a round
  // trip too: the saved text is stable.
  original.Step(600, 0x8);
  const auto later = SaveScene(original.Get(), "sandbox");
  REQUIRE(later.has_value());
  CHECK(later->find("\"ActiveEffects\"") != std::string::npos);
  testing::RampageTestWorld resumed;
  REQUIRE(LoadScene(resumed.Get(), *later, SceneLoadOptions{}));
  CHECK(SaveScene(resumed.Get(), "sandbox").value() == *later);
}

bool Loads(const std::string& text, const SceneLoadOptions& options)
    SE_REQUIRES(g_sim_owner) {
  testing::RampageTestWorld w;
  return LoadScene(w.Get(), text, options).has_value();
}

TEST_CASE("Malformed scenes are errors", "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  const SceneLoadOptions prefabs = MemoryPrefabs({
      {"unit", std::string(kUnitPrefab)},
      {"nested", R"({"format": "se-prefab-v1", "prefab": "unit"})"},
      {"bad", R"({"format": "se-prefab-v1", "components": [1]})"},
  });
  const std::string guid = R"("guid": "00000000-0000-0000-0000-00000000000a")";
  CHECK(Loads(Scene("{" + guid + "}"), prefabs));
  CHECK_FALSE(Loads(R"({"format": "se-scene-v0"})", prefabs));
  CHECK_FALSE(Loads(R"({"format": "se-scene-v1", "extra": 1})", prefabs));
  CHECK_FALSE(Loads(
      R"({"format": "se-scene-v1", "settings": {"gravity": 9.8}})", prefabs));
  CHECK_FALSE(Loads(Scene("{}"), prefabs));
  CHECK_FALSE(Loads(Scene(R"({"guid": "not-a-guid"})"), prefabs));
  CHECK_FALSE(Loads(Scene("{" + guid + "}, {" + guid + "}"), prefabs));
  CHECK_FALSE(
      Loads(Scene("{" + guid + R"(, "components": {"Wings": {}}})"), prefabs));
  CHECK_FALSE(Loads(Scene("{" + guid + R"(, "components": {"Spawned": {}}})"),
                    prefabs));
  CHECK_FALSE(Loads(Scene("{" + guid + R"(, "components": {"PathCache": {}}})"),
                    prefabs));
  CHECK_FALSE(
      Loads(Scene("{" + guid + R"(, "components": {"Team": {"id": 3.5}}})"),
            prefabs));
  CHECK_FALSE(
      Loads(Scene("{" + guid + R"(, "components": {"Stats": {"HP": "lots"}}})"),
            prefabs));
  CHECK_FALSE(Loads(Scene("{" + guid + R"(, "prefab": "nested"})"), prefabs));
  CHECK_FALSE(Loads(Scene("{" + guid + R"(, "prefab": "bad"})"), prefabs));
  CHECK_FALSE(Loads(Scene("{" + guid + R"(, "prefab": "missing"})"), prefabs));
  CHECK_FALSE(Loads(Scene("{" + guid + R"(, "colour": "red"})"), prefabs));
}

}  // namespace
}  // namespace se::rampage
