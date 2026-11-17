// Copyright 2026 The simple-engine-2 Authors.
//
// M11a's formats (docs/milestone-specs.md "M11a"): the scene writer's
// prefab mode (load, save and load again give identical Worlds for every
// scene fixture; the sandbox saves back to its own content; kFlatten is
// M3's writer byte for byte), prefab documents, the rebase of instances on
// a prefab change, the baseline, and the removal rule beside FinishLoad.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/json.h"
#include "game_rampage/scene.h"
#include "rampage/support/rampage_session.h"
#include "rampage/support/test_world.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "support/test_roles.h"

namespace se::rampage {
namespace {

constexpr std::string_view kEnemyPrefab = "prefabs/enemy.prefab.json";
constexpr std::string_view kPlayerPrefab = "prefabs/player.prefab.json";

// The M11 scene fixtures (tests/fixtures/scenes), then the sandbox.
const std::vector<std::string>& SceneFixtures() {
  static const auto* const kFixtures = new std::vector<std::string>{
      "removed_component", "added_component", "nested_member", "stats_by_name",
      "no_prefab",         "camera",          "sprite_tint",
  };
  return *kFixtures;
}

std::string FixturePath(const std::string& name) {
  return testing::RampageFixtureDir() + "/scenes/" + name + ".scene.json";
}

std::string SandboxPath() {
  return testing::RampageAssetRoot() + "/scenes/sandbox.scene.json";
}

// Every fixture's path, the sandbox last.
std::vector<std::pair<std::string, std::string>> AllScenes() {
  std::vector<std::pair<std::string, std::string>> out;
  for (const std::string& name : SceneFixtures()) {
    out.emplace_back(name, FixturePath(name));
  }
  out.emplace_back("sandbox", SandboxPath());
  return out;
}

SceneLoadOptions AssetOptions() {
  SceneLoadOptions options;
  options.asset_root = testing::RampageAssetRoot();
  return options;
}

SceneWriteOptions PrefabMode() {
  SceneWriteOptions options;
  options.mode = SceneWriteMode::kPrefabs;
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

SceneWriteOptions MemoryPrefabMode(const SceneLoadOptions& load) {
  SceneWriteOptions options;
  options.mode = SceneWriteMode::kPrefabs;
  options.resolver = load.resolver;
  return options;
}

std::string ReadFile(const std::string& path) {
  auto text = ReadTextFile(path);
  REQUIRE(text.has_value());
  // Git may check text files out with CRLF line ends.
  std::erase(*text, '\r');
  return *text;
}

json::Value ParseText(std::string_view text) {
  auto value = json::Parse(text);
  REQUIRE(value.has_value());
  return *value;
}

entt::entity ByName(const World& world, std::string_view name) {
  for (const auto [e, n] : world.CReg().view<const Name>().each()) {
    if (n.value == name) return e;
  }
  return entt::null;
}

// The entity whose "name" is |name| in a scene document.
const json::Value* EntityNamed(const json::Value& doc, std::string_view name) {
  const json::Value* entities = doc.Find("entities");
  if (entities == nullptr) return nullptr;
  for (const json::Value& entity : entities->Items()) {
    const json::Value* n = entity.Find("name");
    if (n != nullptr && n->Text() == name) return &entity;
  }
  return nullptr;
}

std::string Flatten(const World& world) {
  auto text = SaveScene(world, "check");
  REQUIRE(text.has_value());
  return *text;
}

// Equal Hash, equal Digests and equal kFlatten text; with |links|, equal
// prefab links too (kFlatten drops them).
void RequireSameWorld(const World& a, const World& b, bool links = true) {
  CHECK(a.Hash() == b.Hash());
  CHECK(a.Digests() == b.Digests());
  CHECK(Flatten(a) == Flatten(b));
  if (!links) return;
  std::map<Guid, std::string> links_a;
  for (const auto [e, id, link] :
       a.CReg().view<const IdComponent, const PrefabInstance>().each()) {
    links_a.emplace(id.guid, link.prefab);
  }
  std::map<Guid, std::string> links_b;
  for (const auto [e, id, link] :
       b.CReg().view<const IdComponent, const PrefabInstance>().each()) {
    links_b.emplace(id.guid, link.prefab);
  }
  CHECK(links_a == links_b);
}

// An EntityComponentsJson's component, parsed; null when it has none.
json::Value ComponentOf(const EntityComponentsJson& entity,
                        std::string_view name) {
  for (const auto& [component, text] : entity.components) {
    if (component == name) return ParseText(text);
  }
  return json::Value();
}

// A stat of an EntityComponentsJson's Stats (registry form), or nullopt.
std::optional<double> StatOf(const EntityComponentsJson& entity,
                             std::string_view stat) {
  const json::Value stats = ComponentOf(entity, "Stats");
  const json::Value* values = stats.Find("values");
  if (values == nullptr) return std::nullopt;
  for (const json::Value& item : values->Items()) {
    if (item.Find("id")->AsInt64() ==
        static_cast<std::int64_t>(StatIdOf(stat))) {
      return item.Find("value")->AsDouble();
    }
  }
  return std::nullopt;
}

TEST_CASE(
    "scene.Load, save and load again give identical Worlds for every scene "
    "fixture",
    "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  for (const auto& [name, path] : AllScenes()) {
    for (const SceneWriteMode mode :
         {SceneWriteMode::kFlatten, SceneWriteMode::kPrefabs}) {
      INFO(name << (mode == SceneWriteMode::kFlatten ? " kFlatten"
                                                     : " kPrefabs"));
      testing::RampageTestWorld first;
      const auto loaded = LoadSceneFile(first.Get(), path, AssetOptions());
      REQUIRE(loaded.has_value());
      SceneWriteOptions options = PrefabMode();
      options.mode = mode;
      const auto saved = SaveScene(first.Get(), loaded->name, options);
      REQUIRE(saved.has_value());
      testing::RampageTestWorld second;
      const auto again = LoadScene(second.Get(), *saved, AssetOptions());
      REQUIRE(again.has_value());
      CHECK(again->entities == loaded->entities);
      RequireSameWorld(first.Get(), second.Get(),
                       mode == SceneWriteMode::kPrefabs);
      // The second save is the first.
      CHECK(SaveScene(second.Get(), loaded->name, options).value() == *saved);
    }
  }
}

TEST_CASE("scene.A mid-game World saves with prefabs and loads back",
          "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld played;
  REQUIRE(LoadSceneFile(played.Get(), SandboxPath(), AssetOptions()));
  // Path progress, positions, running effects and cooldowns.
  played.Step(600, 0x8);
  const auto saved = SaveScene(played.Get(), "sandbox", PrefabMode());
  REQUIRE(saved.has_value());
  const json::Value doc = ParseText(*saved);
  const json::Value* player = EntityNamed(doc, "player");
  REQUIRE(player != nullptr);
  REQUIRE(player->Find("prefab") != nullptr);
  CHECK(player->Find("components") == nullptr);
  testing::RampageTestWorld resumed;
  REQUIRE(LoadScene(resumed.Get(), *saved, AssetOptions()));
  // PrevTransform (hashed) restarts at the load, so the texts compare.
  CHECK(Flatten(resumed.Get()) == Flatten(played.Get()));
  CHECK(SaveScene(resumed.Get(), "sandbox", PrefabMode()).value() == *saved);
}

TEST_CASE("scene.The sandbox scene saves back to its own content",
          "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  REQUIRE(LoadSceneFile(w.Get(), SandboxPath(), AssetOptions()));
  const auto saved = SaveScene(w.Get(), "sandbox", PrefabMode());
  REQUIRE(saved.has_value());
  const json::Value written = ParseText(*saved);
  const json::Value file = ParseText(ReadFile(SandboxPath()));
  INFO(*saved);
  CHECK(json::Equal(written, file));
  // Only Path overrides: no Transform, no derived component.
  for (const json::Value& entity : written.Find("entities")->Items()) {
    CHECK(entity.Find("components") == nullptr);
    if (const json::Value* overrides = entity.Find("overrides")) {
      REQUIRE(overrides->Members().size() == 1);
      REQUIRE(overrides->Members().front().key == "Path");
    }
  }
}

TEST_CASE("scene.Flatten mode writes what M3's writer wrote",
          "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  // tests/fixtures/scenes/flatten holds the M3 writer's output for each
  // scene (written by the M3 SaveScene before the prefab mode existed).
  for (const auto& [name, path] : AllScenes()) {
    INFO(name);
    testing::RampageTestWorld w;
    const auto loaded = LoadSceneFile(w.Get(), path, AssetOptions());
    REQUIRE(loaded.has_value());
    const std::string expected =
        ReadFile(testing::RampageFixtureDir() + "/scenes/flatten/" + name +
                 ".scene.json");
    CHECK(SaveScene(w.Get(), loaded->name).value() == expected);
    SceneWriteOptions flatten;
    flatten.mode = SceneWriteMode::kFlatten;
    flatten.asset_root = "unused";
    CHECK(SaveScene(w.Get(), loaded->name, flatten).value() == expected);
  }
}

TEST_CASE("scene.The loader links instances to their prefabs",
          "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  std::vector<std::pair<std::string, std::string>> reported;
  SceneLoadOptions options = AssetOptions();
  options.on_prefab = [&reported](std::string_view ref, std::string_view text) {
    reported.emplace_back(std::string(ref), std::string(text));
  };
  REQUIRE(LoadSceneFile(w.Get(), FixturePath("no_prefab"), options));
  // Once per prefab, with its text.
  REQUIRE(reported.size() == 1);
  CHECK(reported[0].first == kEnemyPrefab);
  CHECK(reported[0].second == ReadTextFile(testing::RampageAssetRoot() + "/" +
                                           std::string(kEnemyPrefab))
                                  .value());
  const entt::registry& reg = w.Get().CReg();
  const entt::entity wall = ByName(w.Get(), "wall");
  CHECK_FALSE(reg.all_of<PrefabInstance>(wall));
  const entt::entity enemy = ByName(w.Get(), "enemy beside the wall");
  REQUIRE(reg.all_of<PrefabInstance>(enemy));
  CHECK(reg.get<PrefabInstance>(enemy).prefab == kEnemyPrefab);

  // A scene may not set the link itself.
  testing::RampageTestWorld bad;
  CHECK_FALSE(LoadScene(bad.Get(), R"({"format": "se-scene-v1", "entities": [
      {"guid": "00000000-0000-0000-0000-00000000000a",
       "components": {"PrefabInstance": {"prefab": "x"}}}]})",
                        AssetOptions()));

  // The link is not hashed: unlinking changes no hash.
  const WorldHash before = w.Get().Hash();
  w.Reg().remove<PrefabInstance>(enemy);
  CHECK(w.Get().Hash() == before);
}

TEST_CASE("scene.Overrides survive a prefab change", "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  const std::string old_prefab = R"({"format": "se-prefab-v1", "components": {
      "Transform": {}, "Team": {"id": "enemy"},
      "Stats": {"HP": 100, "Speed": 500},
      "Path": {"points": [], "loop": true}}})";
  const std::string new_prefab = R"({"format": "se-prefab-v1", "components": {
      "Transform": {}, "Team": {"id": "enemy"},
      "Stats": {"HP": 150, "Speed": 600},
      "Path": {"points": [{"x": 1, "y": 1}], "loop": true}}})";
  const SceneLoadOptions options = MemoryPrefabs({{"unit", old_prefab}});
  testing::RampageTestWorld w;
  REQUIRE(LoadScene(w.Get(), R"({"format": "se-scene-v1", "entities": [
      {"guid": "00000000-0000-0001-0000-000000000001", "name": "fast",
       "prefab": "unit",
       "overrides": {"Stats": {"Speed": 250},
                     "Path": {"points": [{"x": 10, "y": 20},
                                         {"x": 30, "y": 40}]}}},
      {"guid": "00000000-0000-0001-0000-000000000002", "name": "plain",
       "prefab": "unit"},
      {"guid": "00000000-0000-0001-0000-000000000003", "name": "pinned",
       "prefab": "unit",
       "overrides": {"Stats": {"HP": 7, "Speed": 8},
                     "Path": {"points": [{"x": 5, "y": 5}]}}},
      {"guid": "00000000-0000-0001-0000-000000000004", "name": "loose",
       "components": {"Stats": {"HP": 1}}}]})",
                    options));
  const auto rebased =
      RebasePrefabInstances(w.Get(), "unit", old_prefab, new_prefab, options);
  REQUIRE(rebased.has_value());
  // "pinned" overrides everything that changed; "loose" is no instance.
  REQUIRE(rebased->size() == 2);
  const EntityComponentsJson& fast = (*rebased)[0];
  CHECK(fast.guid == Guid{1, 1});
  CHECK(StatOf(fast, "HP") == 150.0);
  CHECK(StatOf(fast, "Speed") == 250.0);
  const json::Value path = ComponentOf(fast, "Path");
  REQUIRE(path.IsObject());
  REQUIRE(path.Find("points")->Items().size() == 2);
  CHECK(path.Find("points")->Items()[1].Find("y")->AsDouble() == 40.0);
  // R12 still places it: on its own first point.
  const json::Value transform = ComponentOf(fast, "Transform");
  REQUIRE(transform.IsObject());
  CHECK(transform.Find("pos")->Find("x")->AsDouble() == 10.0);
  const EntityComponentsJson& plain = (*rebased)[1];
  CHECK(plain.guid == Guid{1, 2});
  CHECK(StatOf(plain, "HP") == 150.0);
  CHECK(StatOf(plain, "Speed") == 600.0);
  const json::Value plain_path = ComponentOf(plain, "Path");
  REQUIRE(plain_path.IsObject());
  REQUIRE(plain_path.Find("points")->Items().size() == 1);
  // Only what the World holds is listed, in registration order.
  std::vector<std::string> names;
  names.reserve(plain.components.size());
  for (const auto& [name, text] : plain.components) names.push_back(name);
  CHECK(names == std::vector<std::string>{"Transform", "Team", "Stats",
                                          "ActiveEffects", "Path"});

  // A member the new prefab renamed: the override stays, as written.
  const std::string renamed = R"({"format": "se-prefab-v1", "components": {
      "Transform": {}, "Team": {"id": "enemy"},
      "Stats": {"HP": 100, "Velocity": 500},
      "Path": {"points": [], "loop": true}}})";
  const auto kept =
      RebasePrefabInstances(w.Get(), "unit", old_prefab, renamed, options);
  REQUIRE(kept.has_value());
  REQUIRE_FALSE(kept->empty());
  CHECK(kept->front().guid == Guid{1, 1});
  CHECK(StatOf(kept->front(), "Speed") == 250.0);
  CHECK(StatOf(kept->front(), "Velocity") == 500.0);

  // An unchanged prefab rebases nothing.
  const auto same =
      RebasePrefabInstances(w.Get(), "unit", old_prefab, old_prefab, options);
  REQUIRE(same.has_value());
  CHECK(same->empty());
  CHECK(RebasePrefabInstances(w.Get(), "other", old_prefab, new_prefab, options)
            ->empty());
}

TEST_CASE("scene.A removed prefab component saves as null and stays removed",
          "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  // From the fixture: the override is null in the saved file.
  testing::RampageTestWorld fixture;
  REQUIRE(LoadSceneFile(fixture.Get(), FixturePath("removed_component"),
                        AssetOptions()));
  const json::Value doc = ParseText(
      SaveScene(fixture.Get(), "removed_component", PrefabMode()).value());
  const json::Value* invisible = EntityNamed(doc, "invisible enemy");
  REQUIRE(invisible != nullptr);
  const json::Value* removed =
      invisible->Find("overrides")->Find("SpriteRenderer");
  REQUIRE(removed != nullptr);
  CHECK(removed->IsNull());

  // Removed in the World (as the editor's command does): null, and the
  // load leaves it removed.
  const std::string prefab = R"({"format": "se-prefab-v1", "components": {
      "Transform": {}, "Team": {"id": "enemy"},
      "Collider": {"half_extents": {"x": 4, "y": 4}},
      "Stats": {"HP": 100}}})";
  const SceneLoadOptions options = MemoryPrefabs({{"unit", prefab}});
  testing::RampageTestWorld w;
  REQUIRE(LoadScene(w.Get(), R"({"format": "se-scene-v1", "entities": [
      {"guid": "00000000-0000-0001-0000-000000000001", "name": "a",
       "prefab": "unit"}]})",
                    options));
  const entt::entity a = ByName(w.Get(), "a");
  w.Reg().remove<Collider>(a);
  w.Reg().remove<Stats>(a);
  for (const std::string_view derived : RemovedWith("Stats")) {
    w.Reg().storage(w.Registry().FindByName(derived)->id)->remove(a);
  }
  const auto saved = SaveScene(w.Get(), "s", MemoryPrefabMode(options));
  REQUIRE(saved.has_value());
  const json::Value overrides =
      *EntityNamed(ParseText(*saved), "a")->Find("overrides");
  CHECK(json::Equal(overrides,
                    ParseText(R"({"Collider": null, "Stats": null})")));
  testing::RampageTestWorld reloaded;
  REQUIRE(LoadScene(reloaded.Get(), *saved, options));
  RequireSameWorld(w.Get(), reloaded.Get());
  const entt::entity b = ByName(reloaded.Get(), "a");
  CHECK_FALSE(reloaded.Get().CReg().any_of<Collider, Stats, ActiveEffects>(b));

  // Across a rebase: the new prefab's other change arrives, the removed
  // components stay removed.
  const std::string changed = R"({"format": "se-prefab-v1", "components": {
      "Transform": {}, "Team": {"id": "friendly"},
      "Collider": {"half_extents": {"x": 8, "y": 8}},
      "Stats": {"HP": 300}}})";
  const auto rebased =
      RebasePrefabInstances(w.Get(), "unit", prefab, changed, options);
  REQUIRE(rebased.has_value());
  REQUIRE(rebased->size() == 1);
  CHECK(ComponentOf(rebased->front(), "Collider").IsNull());
  CHECK(ComponentOf(rebased->front(), "Stats").IsNull());
  const json::Value team = ComponentOf(rebased->front(), "Team");
  REQUIRE(team.IsObject());
  REQUIRE(team.Find("id")->Text() == "friendly");
  // A new prefab without the component: still removed, nothing to do.
  const std::string dropped = R"({"format": "se-prefab-v1", "components": {
      "Transform": {}, "Team": {"id": "enemy"}}})";
  const auto none =
      RebasePrefabInstances(w.Get(), "unit", prefab, dropped, options);
  REQUIRE(none.has_value());
  CHECK(none->empty());
}

TEST_CASE("scene.Instances changed in the World save as their overrides",
          "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  REQUIRE(LoadSceneFile(w.Get(), SandboxPath(), AssetOptions()));
  const entt::entity enemy = ByName(w.Get(), "enemy 3");
  // A moved Transform, a changed stat, a stat added by name, a nested
  // sprite member and an added component.
  w.Reg().get<Transform>(enemy).pos = Vec2{5.0F, 6.0F};
  // PrevTransform (hashed, not saved) restarts at a load: keep it equal.
  w.Reg().get<PrevTransform>(enemy).pos = Vec2{5.0F, 6.0F};
  w.SetStat(enemy, "HP", 42.0F);
  w.SetStat(enemy, "Luck", 3.0F);
  w.Reg().get<SpriteRenderer>(enemy).region.w = 40.0F;
  w.Reg().emplace<KinematicBody>(enemy, Vec2{1.0F, 2.0F});
  const auto saved = SaveScene(w.Get(), "sandbox", PrefabMode());
  REQUIRE(saved.has_value());
  const json::Value overrides =
      *EntityNamed(ParseText(*saved), "enemy 3")->Find("overrides");
  CHECK(json::Equal(*overrides.Find("Stats"),
                    ParseText(R"({"HP": 42, "Luck": 3})")));
  CHECK(json::Equal(*overrides.Find("SpriteRenderer"),
                    ParseText(R"({"region": {"w": 40}})")));
  CHECK(json::Equal(*overrides.Find("KinematicBody"),
                    ParseText(R"({"velocity": {"x": 1, "y": 2}})")));
  CHECK(json::Equal(*overrides.Find("Transform"),
                    ParseText(R"({"pos": {"x": 5, "y": 6}})")));
  CHECK(overrides.Find("ActiveEffects") == nullptr);
  testing::RampageTestWorld reloaded;
  REQUIRE(LoadScene(reloaded.Get(), *saved, AssetOptions()));
  RequireSameWorld(w.Get(), reloaded.Get());
}

TEST_CASE("scene.A prefab baseline is a bare instance", "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  REQUIRE(LoadSceneFile(w.Get(), SandboxPath(), AssetOptions()));
  const Guid player =
      w.Get().CReg().get<IdComponent>(ByName(w.Get(), "player")).guid;
  const auto baseline = PrefabBaseline(w.Get(), player, AssetOptions());
  REQUIRE(baseline.has_value());
  CHECK(baseline->guid == player);
  // The player has no overrides: its baseline is what it holds.
  const auto bare = PrefabBaseline(
      w.Get(), w.Get().CReg().get<IdComponent>(ByName(w.Get(), "enemy 1")).guid,
      AssetOptions());
  REQUIRE(bare.has_value());
  const json::Value bare_path = ComponentOf(*bare, "Path");
  REQUIRE(bare_path.IsObject());
  CHECK(bare_path.Find("points")->Items().empty());
  CHECK(StatOf(*bare, "Speed") == 500.0);
  CHECK(ComponentOf(*bare, "PrefabInstance").IsNull());
  // Errors: no such entity, no prefab.
  CHECK(PrefabBaseline(w.Get(), Guid{9, 9}, AssetOptions()).error().code ==
        ErrorCode::kNotFound);
  testing::RampageTestWorld loose;
  REQUIRE(LoadSceneFile(loose.Get(), FixturePath("no_prefab"), AssetOptions()));
  const Guid wall =
      loose.Get().CReg().get<IdComponent>(ByName(loose.Get(), "wall")).guid;
  CHECK(PrefabBaseline(loose.Get(), wall, AssetOptions()).error().code ==
        ErrorCode::kInvalidArgument);
  // Through the reloaded instance, the same.
  testing::RampageTestWorld other;
  REQUIRE(LoadScene(other.Get(),
                    SaveScene(w.Get(), "sandbox", PrefabMode()).value(),
                    AssetOptions()));
  const auto again = PrefabBaseline(other.Get(), player, AssetOptions());
  REQUIRE(again.has_value());
  CHECK(again->components == baseline->components);
}

TEST_CASE("scene.A prefab document saves back to its file",
          "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  for (const std::string_view ref : {kEnemyPrefab, kPlayerPrefab}) {
    INFO(ref);
    const std::string path =
        testing::RampageAssetRoot() + "/" + std::string(ref);
    const std::string text = ReadFile(path);
    testing::RampageTestWorld doc;
    const auto loaded =
        LoadPrefabDocument(doc.Get(), text, ref, AssetOptions());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entities == 1);
    REQUIRE(doc.Get().EntityCount() == 1);
    const auto saved = SavePrefabDocument(doc.Get(), ref, loaded->name);
    REQUIRE(saved.has_value());
    INFO(*saved);
    CHECK(json::Equal(ParseText(*saved), ParseText(text)));
    // Loading the saved document gives the same World.
    testing::RampageTestWorld again;
    REQUIRE(LoadPrefabDocument(again.Get(), *saved, ref, AssetOptions()));
    RequireSameWorld(doc.Get(), again.Get());
    CHECK(SavePrefabDocument(again.Get(), ref, loaded->name).value() == *saved);
  }

  // An edit changes only the member it touched.
  const std::string text =
      ReadFile(testing::RampageAssetRoot() + "/" + std::string(kEnemyPrefab));
  testing::RampageTestWorld doc;
  REQUIRE(LoadPrefabDocument(doc.Get(), text, kEnemyPrefab, AssetOptions()));
  const entt::entity root = *doc.Get().Find(PrefabRootGuid(kEnemyPrefab));
  doc.SetStat(root, "HP", 250.0F);
  doc.Reg().get<SpriteRenderer>(root).region.x = 48.0F;
  doc.Reg().remove<Team>(root);
  doc.Reg().emplace<KinematicBody>(root);
  const json::Value saved =
      ParseText(SavePrefabDocument(doc.Get(), kEnemyPrefab, "enemy").value());
  json::Value expected = ParseText(text);
  json::MergePatch(expected, ParseText(R"({"components": {
      "Stats": {"HP": 250},
      "SpriteRenderer": {"region": {"x": 48}},
      "Team": null,
      "KinematicBody": {"velocity": {"x": 0, "y": 0}}}})"));
  CHECK(json::Equal(saved, expected));
  // A World without that prefab's root.
  CHECK(SavePrefabDocument(doc.Get(), "prefabs/other.prefab.json", "x")
            .error()
            .code == ErrorCode::kNotFound);

  // A root built in place (no source text): written whole, and it loads to
  // the same World.
  testing::RampageTestWorld built;
  const entt::entity made =
      built.Get().Create(PrefabRootGuid("prefabs/made.prefab.json"));
  built.Reg().emplace<Name>(made, "made");
  built.Reg().emplace<Transform>(made, Vec2{3.0F, 4.0F}, 0.0F, Vec2{1.0F, 1.0F},
                                 Tick{0});
  built.SetStat(made, "HP", 5.0F);
  built.Finish();
  const auto whole =
      SavePrefabDocument(built.Get(), "prefabs/made.prefab.json", "made");
  REQUIRE(whole.has_value());
  CHECK(ParseText(*whole).Find("components")->Find("ActiveEffects") == nullptr);
  testing::RampageTestWorld from_file;
  REQUIRE(LoadPrefabDocument(from_file.Get(), *whole,
                             "prefabs/made.prefab.json", AssetOptions()));
  RequireSameWorld(built.Get(), from_file.Get());
}

TEST_CASE("scene.A prefab root's Guid is fixed", "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  // The ADR-114 hash, pinned: a prefab document hashes the same everywhere.
  const Guid enemy = PrefabRootGuid(kEnemyPrefab);
  CHECK_FALSE(enemy.IsNil());
  CHECK(enemy == PrefabRootGuid(std::string(kEnemyPrefab)));
  CHECK(enemy != PrefabRootGuid(kPlayerPrefab));
  CHECK(ToString(enemy) == ToString(PrefabRootGuid(kEnemyPrefab)));
  REQUIRE(ToString(enemy) == "b2165603-9f35-fcd3-050a-e0259fda9263");
  const std::string text =
      ReadFile(testing::RampageAssetRoot() + "/" + std::string(kEnemyPrefab));
  testing::RampageTestWorld a;
  REQUIRE(LoadPrefabDocument(a.Get(), text, kEnemyPrefab, AssetOptions()));
  testing::RampageTestWorld b;
  REQUIRE(LoadPrefabDocument(b.Get(), text, kEnemyPrefab, AssetOptions()));
  CHECK(a.Get().Find(enemy).has_value());
  CHECK(a.Get().Hash() == b.Get().Hash());
  // The root is the prefab, not an instance of it.
  CHECK_FALSE(a.Get().CReg().all_of<PrefabInstance>(*a.Get().Find(enemy)));
}

TEST_CASE("scene.Every component FinishLoad adds is removed with its source",
          "[rampage][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  const entt::entity e = w.Add({10.0F, 10.0F});
  w.Reg().emplace<Path>(e, std::vector<Vec2>{{0.0F, 0.0F}, {5.0F, 0.0F}});
  w.SetStat(e, "HP", 10.0F);
  EffectSource source;
  source.polled = true;
  w.Reg().emplace<EffectSource>(e, source);
  const ComponentRegistry& registry = w.Registry();
  const auto held = [&w, &registry](entt::entity entity) {
    std::set<std::string> out;
    for (const ComponentInfo& info : registry.All()) {
      const auto* storage = w.Get().CReg().storage(info.id);
      if (storage != nullptr && storage->contains(entity)) {
        out.emplace(info.name);
      }
    }
    return out;
  };
  const std::set<std::string> before = held(e);
  w.Finish();
  const std::set<std::string> after = held(e);
  std::vector<std::string> added;
  std::ranges::set_difference(after, before, std::back_inserter(added));
  // Spawned and PrevTransform are the spawn's, not derived.
  std::erase(added, "PrevTransform");
  REQUIRE(added.size() == 3);
  for (const std::string& component : added) {
    INFO(component);
    bool listed = false;
    for (const std::string& owner_name : before) {
      const auto with = RemovedWith(owner_name);
      listed = listed || std::ranges::find(with, component) != with.end();
    }
    CHECK(listed);
  }
  // Removing a source with what it takes: FinishLoad adds nothing back.
  for (const std::string_view from : {"Path", "Stats", "EffectSource"}) {
    INFO(from);
    REQUIRE_FALSE(RemovedWith(from).empty());
    w.Reg().storage(registry.FindByName(from)->id)->remove(e);
    for (const std::string_view derived : RemovedWith(from)) {
      w.Reg().storage(registry.FindByName(derived)->id)->remove(e);
    }
    const std::set<std::string> removed = held(e);
    w.Finish();
    CHECK(held(e) == removed);
  }
  CHECK(RemovedWith("Team").empty());
  for (const std::string_view fixed :
       {"IdComponent", "Name", "Spawned", "PrefabInstance", "Transform"}) {
    CHECK_FALSE(Removable(fixed));
  }
  for (const std::string_view free : {"Path", "Stats", "SpriteRenderer"}) {
    CHECK(Removable(free));
  }
}

TEST_CASE("scene.A stat name can be forgotten", "[rampage][scene]") {
  StatNames names;
  const StatId luck = names.Add("Luck");
  REQUIRE(names.Name(luck) == "Luck");
  CHECK(names.Remove(luck));
  CHECK(names.Name(luck).empty());
  CHECK_FALSE(names.Remove(luck));
  CHECK(names.All().empty());
}

}  // namespace
}  // namespace se::rampage
