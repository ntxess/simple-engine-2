// Copyright 2026 The simple-engine-2 Authors.
//
// content_probe: the content test module (tests/support/
// content_test_module.h) as a real game executable, for the M13a crash
// test (session_log_dump_test.py): its states load their texts through the
// lane and the content gate, focus loss pushes "pause" over "play" through
// the SessionLog, and its dumps carry the session stream (the replay file
// without frames: a content bundle and the SessionLog).
//
//   content_probe [runtime options] -- [--start-state ID]
//
// The content root is --content-root (the tests pass tests/fixtures).

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "se_core/error.h"
#include "se_runtime/app.h"
#include "se_runtime/app_config.h"
#include "se_runtime/asset_runtime.h"
#include "se_sim/game_state.h"
#include "support/content_test_module.h"

// Last: SDL_main.h with SDL_MAIN_USE_CALLBACKS, which provides main().
#include "se_runtime/sdl_main.h"

namespace {

std::expected<se::GameSetup, se::Error> MakeProbe(const se::AppConfig& config) {
  se::GameSetup setup;
  setup.module = &se::testing::ContentTestModule();
  setup.title = "content_probe";
  setup.org = "simple-engine-tests";
  setup.app = "content_probe";
  setup.seed = 3;
  setup.tick_rate = 120;
  setup.start_state = std::string(se::testing::kContentPlay);
  for (std::size_t i = 0; i < config.game_args.size(); ++i) {
    if (config.game_args[i] == "--start-state" &&
        i + 1 < config.game_args.size()) {
      setup.start_state = config.game_args[++i];
    } else {
      return se::Fail(
          se::ErrorCode::kInvalidArgument,
          "content_probe: unknown option '" + config.game_args[i] + "'");
    }
  }
  setup.state_assets = [](std::string_view id) {
    se::StateAssets assets;
    assets.all_textures = false;
    assets.all_sounds = false;
    assets.texts = {std::string(se::testing::ContentStateText(id))};
    return assets;
  };
  setup.text_dependencies = &se::testing::ContentTestDependencies;
  setup.on_focus_lost =
      [](std::string_view top) -> std::optional<se::StateCommand> {
    if (top != se::testing::kContentPlay) return std::nullopt;
    return se::StateCommand{se::StackOp::kPush,
                            std::string(se::testing::kContentPause)};
  };
  return setup;
}

}  // namespace

SE_DEFINE_SDL_APP(&MakeProbe)
