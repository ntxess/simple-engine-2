// Copyright 2026 The simple-engine-2 Authors.
//
// EditorGame: what the editor needs from a game besides its GameModule
// (docs/milestone-specs.md M10a; ADR-097). The editor never links a game
// module (se_editor FORBIDs game_rampage); the editor executable fills
// this from the game's own functions (rampage's scene loader, M3).

#ifndef SE_EDITOR_EDITOR_GAME_H_
#define SE_EDITOR_EDITOR_GAME_H_

#include <expected>
#include <functional>
#include <string>
#include <string_view>

#include "se_core/error.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_sim/game_module.h"

namespace se::editor {

struct EditorGame {
  const GameModule* module = nullptr;
  // Loads a scene document's text into an empty World whose MutationGate
  // is open: settings, then entities (rampage: LoadScene). |asset_root| is
  // where the document's prefab references resolve.
  std::function<std::expected<void, Error>(World& world, std::string_view text,
                                           const std::string& asset_root)>
      load_scene;
  // Writes a World as a scene document (rampage: SaveScene; prefabs
  // flattened, so the text loads without its prefabs).
  std::function<std::expected<std::string, Error>(const World& world,
                                                  std::string_view name)>
      save_scene;
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
