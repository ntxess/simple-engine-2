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
// in the World's StatNames; the registry form ({"values": [{"id": ...}]})
// is accepted as well. SaveScene writes the named form. A SpriteRenderer's
// texture is written by name too ("texture": "player", a key of
// config/texture.json) and stored as its TextureKeyOf key, the names going
// to the World's TextureNames.

#ifndef GAME_RAMPAGE_SCENE_H_
#define GAME_RAMPAGE_SCENE_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

#include "game_rampage/components.h"
#include "game_rampage/json.h"
#include "se_core/error.h"
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

// The World as an se-scene-v1 document: settings, then every entity in
// spawn order with its serialized components (prefabs flattened).
[[nodiscard]] std::expected<std::string, Error> SaveScene(
    const World& world, std::string_view name);

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

// Turns the named Stats and effect-stat forms of one component's JSON into
// the registry form, recording the names. |component| is the registry name.
std::expected<void, Error> StatNamesToIds(std::string_view component,
                                          json::Value& value, StatNames& names);

// A whole file, or an error naming it.
[[nodiscard]] std::expected<std::string, Error> ReadTextFile(
    const std::string& path);

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_SCENE_H_
