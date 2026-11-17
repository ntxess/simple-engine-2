// Copyright 2026 The simple-engine-2 Authors.
//
// The rampage executable (docs/milestone-specs.md M5, M7, M13b): the game
// through game_rampage's GameModule at 120 Hz on SDL's main callbacks,
// drawn by Renderer2D: every state's Worlds (the stack's render sources,
// each through SandboxCamera) with the textures of config/texture.json. A
// session starts at the Title (M13b): Enter plays, Escape pauses and
// resumes, Space in the Pause returns to the Title, Enter at the GameOver
// retries and Escape there returns to the Title; Escape at the Title ends
// the game. Losing the window's focus while playing pauses. The window
// title shows the top state's id, the tick and the lifecycle state.
//
//   rampage [runtime options] [-- [--scene FILE] [--start-state ID]]
//
// Runtime options are AppConfig's (se_runtime/app_config.h), for example
// --frames N to exit through the two-phase quit after N presented frames.
// --scene names the Gameplay (and sandbox) scene relative to the content
// root (default assets/scenes/sandbox.scene.json); the content root is the
// first directory from the executable's upwards that holds the default
// scene. --start-state picks the state the App pushes at boot (default
// "title"; tests also start in "gameplay", or in "sandbox", the one-World
// state of the editor's Play, se_replay and the goldens).
//
// Every state's scene loads as a text through the background lane, with
// the prefabs it names (GameSetup::text_dependencies), before the state is
// created (M13a's content gate): no World steps before its textures,
// sounds and texts are resident.

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/content.h"
#include "game_rampage/flow.h"
#include "game_rampage/module.h"
#include "game_rampage/view.h"
#include "se_core/error.h"
#include "se_core/world.h"
#include "se_platform/fs.h"
#include "se_runtime/app.h"
#include "se_runtime/app_config.h"
#include "se_runtime/asset_runtime.h"
#include "se_sim/game_state.h"

// Last: SDL_main.h with SDL_MAIN_USE_CALLBACKS, which provides main().
#include "se_runtime/sdl_main.h"

namespace {

constexpr std::string_view kDefaultScene = se::rampage::kDefaultGameplayScene;

// What a state loads before it is created: its scene as a text (the lane
// adds the prefabs it names), the textures it draws and the table's sounds.
// The Gameplay and the sandbox may draw any texture of the table (their
// scene is configurable); the Title, Pause and GameOver draw the wallpaper.
se::StateAssets AssetsOf(std::string_view state_id) {
  se::StateAssets assets;
  if (state_id == se::rampage::kSandboxState) {
    assets.texts.push_back(se::rampage::SandboxScene().content_path);
    return assets;
  }
  const std::string scene = se::rampage::FlowSceneOf(state_id);
  if (scene.empty()) return assets;
  assets.texts.push_back(scene);
  if (state_id != se::rampage::kGameplayState) {
    assets.all_textures = false;
    assets.textures.emplace_back("bg");
  }
  return assets;
}

std::expected<se::GameSetup, se::Error> MakeRampage(
    const se::AppConfig& config) {
  std::string scene(kDefaultScene);
  std::string start_state(se::rampage::kTitleState);
  for (std::size_t i = 0; i < config.game_args.size(); ++i) {
    const std::string& arg = config.game_args[i];
    const bool has_value = i + 1 < config.game_args.size();
    if (arg == "--scene" && has_value) {
      scene = config.game_args[++i];
    } else if (arg == "--start-state" && has_value) {
      start_state = config.game_args[++i];
    } else {
      return se::Fail(se::ErrorCode::kInvalidArgument,
                      "rampage: unknown game option '" + arg +
                          "' (expected --scene FILE or --start-state ID)");
    }
  }
  if (start_state != se::rampage::kSandboxState &&
      se::rampage::FlowSceneOf(start_state).empty()) {
    return se::Fail(se::ErrorCode::kInvalidArgument,
                    "rampage: unknown start state '" + start_state +
                        "' (expected title, gameplay, pause, game_over or "
                        "sandbox)");
  }
  se::GameSetup setup;
  setup.module = &se::rampage::RampageModule();
  setup.title = "rampage";
  setup.org = "simple-engine";
  setup.app = "rampage";
  setup.content_marker = std::string(kDefaultScene);
  setup.seed = 1;
  setup.tick_rate = 120;
  // Opaque black, as the legacy Engine clears its window, so the window
  // matches the legacy image of the same tick.
  setup.clear_color = se::ClearColor{0.0F, 0.0F, 0.0F, 1.0F};
  setup.window_width = 1280;
  setup.window_height = 720;
  setup.texture_table = "config/texture.json";
  // Key bindings and sound volumes, and the rules' sounds (M8).
  setup.settings_file = "config/config.json";
  setup.sound_table = "config/sound.json";
  setup.start_state = start_state;
  setup.state_assets = &AssetsOf;
  setup.text_dependencies = [](std::string_view path, std::string_view text) {
    return se::rampage::SceneDependencies(path, text,
                                          se::rampage::kContentAssetDir);
  };
  // Switching to another window while playing pauses (a logged state
  // command, so a replay pauses at the same tick).
  setup.on_focus_lost =
      [](std::string_view top) -> std::optional<se::StateCommand> {
    if (top != se::rampage::kGameplayState) return std::nullopt;
    return se::StateCommand{se::StackOp::kPush,
                            std::string(se::rampage::kPauseState)};
  };
  setup.camera = [](const se::World& world) {
    return se::rampage::SandboxCamera(world);
  };
  setup.configure =
      [scene](const se::ContentPaths& paths) -> std::expected<void, se::Error> {
    se::rampage::SceneRequest request;
    request.format = se::rampage::SceneRequest::Format::kScene;
    request.path = se::fs::Join(paths.content_root, scene);
    request.asset_root = se::fs::Join(paths.content_root, "assets");
    request.legacy_root = paths.content_root;
    request.content_path = scene;
    if (!se::fs::Exists(request.path)) {
      return std::unexpected(se::Error{se::ErrorCode::kNotFound,
                                       "rampage: no scene file", request.path,
                                       0, 0});
    }
    se::rampage::SetSandboxScene(std::move(request));
    se::rampage::FlowConfig flow;
    flow.gameplay_scene = scene;
    flow.content_root = paths.content_root;
    se::rampage::SetFlowConfig(std::move(flow));
    return {};
  };
  // The scene for the session header (M12: se_replay --from-dump).
  setup.scene = scene;
  // A --record replay starts from the scene file, as se_replay's do (M13a
  // replaces it with the session's content bundle when the session ran
  // more than its start scene).
  setup.replay_start = [scene](const se::ContentPaths& paths) {
    auto bytes = se::fs::ReadFile(se::fs::Join(paths.content_root, scene));
    return bytes ? std::move(*bytes) : std::vector<std::byte>();
  };
  return setup;
}

}  // namespace

SE_DEFINE_SDL_APP(&MakeRampage)
