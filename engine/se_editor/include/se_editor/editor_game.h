// Copyright 2026 The simple-engine-2 Authors.
//
// EditorGame: what the editor needs from a game besides its GameModule
// (docs/milestone-specs.md M10a, M11 "The scene contract"; ADR-097). The
// editor never links a game module (se_editor FORBIDs game_rampage); the
// editor executable fills this from the game's own functions (rampage's
// scene loader and writer, M3 and M11a).

#ifndef SE_EDITOR_EDITOR_GAME_H_
#define SE_EDITOR_EDITOR_GAME_H_

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_render_core/render_components.h"
#include "se_sim/game_module.h"

namespace se::editor {

// One entity's components in the registry's JSON form, by registered name
// (rampage: EntityComponentsJson).
struct EntityJson {
  Guid guid;
  std::vector<ComponentJson> components;
};

struct EditorGame {
  const GameModule* module = nullptr;
  // Loads a scene document's text into an empty World whose MutationGate
  // is open: settings, then entities (rampage: LoadScene). |asset_root| is
  // where the document's prefab references resolve; |on_prefab|, when set,
  // is called once per prefab the load read, with its text (the editor
  // keeps them: a later change of the file is rebased against them).
  std::function<std::expected<void, Error>(
      World& world, std::string_view text, const std::string& asset_root,
      const std::function<void(std::string_view ref, std::string_view text)>&
          on_prefab)>
      load_scene;
  // Writes a World as a scene document with prefabs flattened (rampage:
  // SaveScene, kFlatten): the Play-start bytes, fault files and dumps,
  // which must load without prefab files, and the content hash.
  std::function<std::expected<std::string, Error>(const World& world,
                                                  std::string_view name)>
      save_scene;
  // Save, Save As, autosave and recovery: with prefab references (rampage:
  // SaveScene, kPrefabs). Empty: save_scene.
  std::function<std::expected<std::string, Error>(
      const World& world, std::string_view name, const std::string& asset_root)>
      save_scene_file;
  // A prefab document: a World with the prefab's one root entity (rampage:
  // LoadPrefabDocument) and its file (SavePrefabDocument). |ref| is the
  // prefab's reference relative to the asset root.
  std::function<std::expected<void, Error>(World& world, std::string_view text,
                                           const std::string& ref,
                                           const std::string& asset_root)>
      load_prefab;
  std::function<std::expected<std::string, Error>(
      const World& world, const std::string& ref, std::string_view name)>
      save_prefab;
  // What a bare instance of the entity's prefab loads to (rampage:
  // PrefabBaseline): the Inspector's override markers and "Revert to
  // prefab".
  std::function<std::expected<EntityJson, Error>(const World& world, Guid guid,
                                                 const std::string& asset_root)>
      prefab_baseline;
  // Every instance of |ref| in |world| as it loads once the prefab's text
  // changes from |old_text| to |new_text| (rampage: RebasePrefabInstances);
  // instances whose result equals what they hold are left out.
  std::function<std::expected<std::vector<EntityJson>, Error>(
      const World& world, std::string_view ref, std::string_view old_text,
      std::string_view new_text, const std::string& asset_root)>
      rebase_instances;
  // The components removing |component| takes with it, and whether it may
  // be removed at all (rampage: RemovedWith, Removable). Empty: nothing
  // goes with it, and everything may be removed.
  std::function<std::vector<std::string>(std::string_view component)>
      removed_with;
  std::function<bool(std::string_view component)> removable;
  // Points the module's start state at a scene file before 'Play game'
  // pushes it (rampage: SetSandboxScene). Empty: 'Play game' runs the
  // start state as the game would.
  std::function<void(const std::string& scene_path,
                     const std::string& asset_root)>
      set_play_game_scene;
  // The camera a World is drawn with (rampage: SandboxCamera); empty: its
  // primary Camera2D, else a default one.
  std::function<Camera2D(const World& world)> camera;
  // Prefab references resolve against <content root>/<asset_dir>.
  std::string asset_dir = "assets";
};

}  // namespace se::editor

#endif  // SE_EDITOR_EDITOR_GAME_H_
