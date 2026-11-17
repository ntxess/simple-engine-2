// Copyright 2026 The simple-engine-2 Authors.

#include "platform/support/test_app.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <expected>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "platform/support/callback_loop.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"
#include "se_runtime/app.h"
#include "se_runtime/app_config.h"
#include "se_runtime/sdl_app.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"

#ifndef SE_TEST_OUT_DIR
#error "SE_TEST_OUT_DIR must name a writable directory"
#endif

namespace se::testing {
namespace {

bool& PlayExitThrows() {
  static bool throws = false;
  return throws;
}

class PlayState final : public GameState {
 public:
  explicit PlayState(const StateEnv& env) : env_(env) {}

  void OnEnter() SE_REQUIRES(g_sim_owner) override {
    WorldDesc desc;
    desc.name = "play";
    desc.kind = env_.world_kind;
    desc.seed = env_.seed;
    desc.tick_rate = env_.tick_rate;
    world_ = std::make_unique<World>(*env_.registry, std::move(desc));
    worlds_[0] = world_.get();
  }

  void OnExit() SE_REQUIRES(g_sim_owner) override {
    if (PlayExitThrows()) {
      throw std::runtime_error("GameState::OnExit test fault");
    }
    worlds_[0] = nullptr;
    world_.reset();
  }

  [[nodiscard]] std::span<World* const> Worlds() const override {
    return world_ ? std::span<World* const>(worlds_)
                  : std::span<World* const>();
  }

 private:
  StateEnv env_;
  std::unique_ptr<World> world_;
  std::array<World*, 1> worlds_{};
};

std::unique_ptr<GameState> MakePlay(const StateEnv& env) {
  return std::make_unique<PlayState>(env);
}

void RegisterStates(GameStateFactory& factory) {
  factory.Register("play", &MakePlay);
}

constexpr std::array<ActionBinding, 2> kBindings = {{
    {"left", "A", "", "dpleft"},
    {"right", "D", "", "dpright"},
}};

GameSetup& NextSetup() {
  static auto* const kSetup = new GameSetup();
  return *kSetup;
}

double& TeardownSec() {
  static double seconds = 0.0;
  return seconds;
}

std::expected<GameSetup, Error> Factory(const AppConfig& /*config*/) {
  return NextSetup();
}

}  // namespace

const GameModule& PlatformTestModule() {
  static const GameModule kModule = {
      .name = "platform_test",
      .register_components = nullptr,
      .register_systems = nullptr,
      .register_states = &RegisterStates,
      .start_state = "play",
      .actions = {.bindings = kBindings},
  };
  return kModule;
}

void SetPlayExitThrows(bool throws) { PlayExitThrows() = throws; }

GameSetup TestGameSetup() {
  GameSetup setup;
  setup.module = &PlatformTestModule();
  setup.title = "se_platform_tests app";
  setup.org = "simple-engine-tests";
  setup.app = "platform";
  setup.window_width = 400;
  setup.window_height = 300;
  setup.clear_color = ClearColor{0.1F, 0.3F, 0.5F, 1.0F};
  return setup;
}

int RunTestApp(GameSetup setup, std::vector<std::string> args) {
  // A message box would wait for a click; CTest sets this too.
  SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SE_NO_MESSAGE_BOX", "1",
                             true);
  // Nothing a test runs is heard: miniaudio's null backend mixes and plays
  // nothing (M8), unless the test chose an output.
  SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SE_AUDIO", "null", false);
  NextSetup() = std::move(setup);
  args.emplace_back("--return-on-quit");
  args.emplace_back("--no-activate");
  // Many apps boot in one test process: no crashpad handler for them (the
  // crash.* tests start it).
  args.emplace_back("--no-crash-capture");
  args.emplace_back("--pref-dir");
  args.emplace_back(std::string(SE_TEST_OUT_DIR) + "/pref");
  CallbackLoop loop;
  loop.init = [](void** appstate, int argc, char** argv) {
    return SdlAppInit(appstate, argc, argv, &Factory);
  };
  loop.iterate = [](void* appstate) { return SdlAppIterate(appstate); };
  loop.event = [](void* appstate, SDL_Event* event) {
    return SdlAppEvent(appstate, event);
  };
  loop.quit = [](void* appstate, SDL_AppResult result) {
    const auto start = std::chrono::steady_clock::now();
    SdlAppQuit(appstate, result);
    TeardownSec() =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
  };
  TeardownSec() = 0.0;
  const int result = RunCallbackLoop(std::move(loop), args);
  NextSetup() = GameSetup();
  if (TeardownSec() >= kTeardownLimitSec) {
    std::fprintf(stderr,
                 "test app: the teardown took %.3f s (the limit is %.1f s)\n",
                 TeardownSec(), kTeardownLimitSec);
    return kSlowTeardown;
  }
  return result;
}

double LastTeardownSec() { return TeardownSec(); }

}  // namespace se::testing
