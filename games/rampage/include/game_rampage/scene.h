// Copyright 2026 The simple-engine-2 Authors.
//
// Scenes and prefabs (se-scene-v1, docs/milestone-specs.md M3b "Content and
// loading"):
//
//   {"format": "se-scene-v1", "name": "...",
//    "settings": {"bounds": {"w": 1920, "h": 1080}, "seed": 1,
//                 "event_watchdog_ms": 36000},
//    "entities": [{"guid": "<36-character Guid>", "name": "...",
//                  "prefab": "prefabs/enemy.prefab.json",
//                  "components": {<registry name>: <json>},
//                  "overrides": {<registry name>: <RFC 7386 merge patch>}}]}
//
//   {"format": "se-prefab-v1", "name": "...",
//    "components": {<registry name>: <json>}}
//
// An entity starts from its prefab's components (no nested prefabs); each
// of its "components" replaces the prefab's component whole, then
// "overrides" is merge-patched over the result: objects merge member by
// member, arrays are replaced whole, and null removes a member (a null
// component removes the component). Prefab references resolve against the
// asset root. Components are created in registration order (Transform
// first) through the ComponentRegistry's JSON hooks only, so glaze stays
// confined to the *_components.cc units. An entity with a Path whose
// Transform sets no "pos" starts at the path's first point (R12).
//
// Stats are written by name: "Stats": {"HP": 100, "Speed": 500} (so an
// override can change one stat), and an effect's stat as its name
// ("stat": "HP"). The loader turns these into StatIds and records the names
// in the World's StatNames (an empty name, or two names with one id, is an
// error); the registry form ({"values": [{"id": ...}]}) is accepted as
// well, in any order (the Stats hook sorts it; a repeated id is an error).
// SaveScene writes the named form. A SpriteRenderer's texture is written by
// name too ("texture": "player", a key of config/texture.json) and stored
// as its TextureKeyOf key, the names going to the World's TextureNames. An
// entity loaded as moved (a legacy scenario's "moving" entities) is saved
// with "Moved": {}, which marks it moved for its first tick (ADR-028).
//
// M11: the loader records an entity's "prefab" as its PrefabInstance. The
// writer has two modes: kFlatten (M3's form, every component whole) and
// kPrefabs, which writes an instance as its "prefab" and the "overrides"
// that give it back on load (ADR-115). A prefab opened as a document is a
// World with one root entity (PrefabRootGuid) and saves back in its file's
// own form; instances are rebased on a prefab change by applying their
// overrides over the new text (ADR-116).

#ifndef GAME_RAMPAGE_SCENE_H_
#define GAME_RAMPAGE_SCENE_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/json.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"

namespace se::rampage {

inline constexpr std::string_view kSceneFormat = "se-scene-v1";
inline constexpr std::string_view kPrefabFormat = "se-prefab-v1";

// The text of the prefab |ref| (a path relative to the asset root).
using PrefabResolver =
    std::function<std::expected<std::string, Error>(std::string_view ref)>;

struct SceneLoadOptions {
  std::string asset_root;
  // Replaces reading <asset_root>/<ref> when set (tests).
  PrefabResolver resolver;
  RulesVersion rules = RulesVersion::kV2;
  // Called once per prefab the load read, with its text (the editor
  // keeps them: a later change of the file is rebased against them).
  std::function<void(std::string_view ref, std::string_view text)> on_prefab;
};

struct SceneSummary {
  std::string name;
  std::size_t entities = 0;
};

// Loads a scene into an empty World (one load per World).
std::expected<SceneSummary, Error> LoadScene(World& world,
                                             std::string_view text,
                                             const SceneLoadOptions& options)
    SE_REQUIRES(g_sim_owner);
std::expected<SceneSummary, Error> LoadSceneFile(
    World& world, const std::string& path, const SceneLoadOptions& options)
    SE_REQUIRES(g_sim_owner);

enum class SceneWriteMode : std::uint8_t {
  // Every entity's components whole (M3's SaveScene): the Play-start
  // bytes, fault files and dumps, which must load without prefab files.
  kFlatten,
  // Prefab instances as "prefab" plus "overrides" (one RFC 7386 merge
  // patch per component against what loading the bare instance gives;
  // null removes a member, or the component), other entities as
  // "components": Save, Save As, autosave and recovery files.
  kPrefabs,
};

struct SceneWriteOptions {
  SceneWriteMode mode = SceneWriteMode::kFlatten;
  // kPrefabs: prefab references resolve here, or through |resolver|.
  std::string asset_root;
  PrefabResolver resolver;
};

// The World as an se-scene-v1 document: settings, then every entity in
// spawn order (Spawned::spawn_serial), as |options.mode| says.
[[nodiscard]] std::expected<std::string, Error> SaveScene(
    const World& world, std::string_view name,
    const SceneWriteOptions& options = {});

// Prefab documents: a prefab opened in the editor is a World with one
// entity whose Guid is PrefabRootGuid(ref) (a fixed hash of the
// reference, so a prefab document hashes the same everywhere).
[[nodiscard]] Guid PrefabRootGuid(std::string_view ref) noexcept;
std::expected<SceneSummary, Error> LoadPrefabDocument(
    World& world, std::string_view text, std::string_view ref,
    const SceneLoadOptions& options) SE_REQUIRES(g_sim_owner);
// The root entity as se-prefab-v1 (its name, its components in the
// scene form: stats and textures by name).
[[nodiscard]] std::expected<std::string, Error> SavePrefabDocument(
    const World& world, std::string_view ref, std::string_view name);

// One entity's components in the registry's JSON form, by registered
// name, in registration order.
struct EntityComponentsJson {
  Guid guid;
  std::vector<std::pair<std::string, std::string>> components;
};

// What a bare instance of the entity's prefab loads to (the writer's
// baseline, and the Inspector's "Revert to prefab").
[[nodiscard]] std::expected<EntityComponentsJson, Error> PrefabBaseline(
    const World& world, Guid guid, const SceneLoadOptions& options);

// Every instance of |ref| in |world| as it loads once the prefab's text
// changes from |old_text| to |new_text|: each instance's overrides,
// taken against the old text, applied over the new one. Instances whose
// result equals what they hold are left out.
[[nodiscard]] std::expected<std::vector<EntityComponentsJson>, Error>
RebasePrefabInstances(const World& world, std::string_view ref,
                      std::string_view old_text, std::string_view new_text,
                      const SceneLoadOptions& options);

// The components removing |component| takes with it (those FinishLoad
// derives from it: PathCache with Path; ActiveEffects with Stats;
// PairCooldowns with EffectSource), and whether it may be removed at all
// (IdComponent, Name, Spawned, PrefabInstance and Transform may not).
[[nodiscard]] std::span<const std::string_view> RemovedWith(
    std::string_view component) noexcept;
[[nodiscard]] bool Removable(std::string_view component) noexcept;

// The parts shared with the legacy scenario loader.

struct SceneSettings {
  ArenaBounds bounds;
  EffectSettings effects;
  std::uint64_t seed = 0;
  RulesVersion rules = RulesVersion::kV2;
};

// Puts the settings, empty StatNames and TextureNames and the collision
// scratch into the World's ctx.
void ApplySceneSettings(World& world, const SceneSettings& settings)
    SE_REQUIRES(g_sim_owner);

// Adds the derived and bookkeeping components a loaded World needs
// (PathCache for paths, ActiveEffects for entities with Stats, PairCooldowns
// for polled sources), then runs FinalizeSpawns.
void FinishLoad(World& world) SE_REQUIRES(g_sim_owner);

// Records the stat |name| in |names| and returns its id. An empty name, or
// a name whose id another name already has, is an error: the World could
// not tell the two stats apart, and the projection could not name them.
std::expected<StatId, Error> InternStatName(StatNames& names,
                                            std::string_view name);

// Turns the named Stats and effect-stat forms of one component's JSON into
// the registry form, recording the names. |component| is the registry name.
std::expected<void, Error> StatNamesToIds(std::string_view component,
                                          json::Value& value, StatNames& names);

// A whole file, or an error naming it.
[[nodiscard]] std::expected<std::string, Error> ReadTextFile(
    const std::string& path);

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_SCENE_H_
