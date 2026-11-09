// Copyright 2026 The simple-engine-2 Authors.
//
// The rampage executable (docs/milestone-specs.md M5, M7): the sandbox
// scene through game_rampage's GameModule at 120 Hz on SDL's main
// callbacks, drawn by Renderer2D: the Sandbox's RenderList (the World at
// the frame's alpha, through SandboxCamera) with the textures of
// config/texture.json. The window title shows the tick and the lifecycle
// state.
//
//   rampage [runtime options] [-- --scene FILE]
//
// Runtime options are AppConfig's (se_runtime/app_config.h), for example
// --frames N to exit through the two-phase quit after N presented frames.
// --scene names a scene file relative to the content root (default
// assets/scenes/sandbox.scene.json); the content root is the first
// directory from the executable's upwards that holds the default scene.

#include <cstddef>
#include <expected>
#include <string>
#include <utility>

#include "game_rampage/module.h"
#include "game_rampage/view.h"
#include "se_core/error.h"
#include "se_core/world.h"
#include "se_platform/fs.h"
#include "se_runtime/app.h"
#include "se_runtime/app_config.h"

// Last: SDL_main.h with SDL_MAIN_USE_CALLBACKS, which provides main().
#include "se_runtime/sdl_main.h"

namespace {

constexpr const char* kDefaultScene = "assets/scenes/sandbox.scene.json";

std::expected<se::GameSetup, se::Error> MakeRampage(
    const se::AppConfig& config) {
  std::string scene = kDefaultScene;
  for (std::size_t i = 0; i < config.game_args.size(); ++i) {
    if (config.game_args[i] == "--scene" && i + 1 < config.game_args.size()) {
      scene = config.game_args[++i];
    } else {
      return se::Fail(se::ErrorCode::kInvalidArgument,
                      "rampage: unknown game option '" + config.game_args[i] +
                          "' (expected --scene FILE)");
    }
  }
  se::GameSetup setup;
  setup.module = &se::rampage::RampageModule();
  setup.title = "rampage";
  setup.org = "simple-engine";
  setup.app = "rampage";
  setup.content_marker = kDefaultScene;
  setup.seed = 1;
  setup.tick_rate = 120;
  setup.clear_color = se::ClearColor{0.10F, 0.12F, 0.20F, 1.0F};
  setup.window_width = 1280;
  setup.window_height = 720;
  setup.texture_table = "config/texture.json";
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
    if (!se::fs::Exists(request.path)) {
      return std::unexpected(se::Error{se::ErrorCode::kNotFound,
                                       "rampage: no scene file", request.path,
                                       0, 0});
    }
    se::rampage::SetSandboxScene(std::move(request));
    return {};
  };
  return setup;
}

}  // namespace

SE_DEFINE_SDL_APP(&MakeRampage)
