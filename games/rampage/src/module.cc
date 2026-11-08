// Copyright 2026 The simple-engine-2 Authors.

#include "game_rampage/module.h"

#include <array>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "game_rampage/components.h"
#include "game_rampage/legacy_scenario.h"
#include "game_rampage/scene.h"
#include "game_rampage/systems.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/world.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"

namespace se::rampage {
namespace {

// Never destroyed (Google style: no non-trivial static destructors).
SceneRequest& PendingScene() {
  static auto* const kRequest = new SceneRequest();
  return *kRequest;
}

// The gameplay of the legacy Sandbox scene: one World loaded from the
// requested scene.
class SandboxState final : public GameState {
 public:
  explicit SandboxState(const StateEnv& env) : env_(env) {}

  void OnEnter() SE_REQUIRES(g_sim_owner) override {
    WorldDesc desc;
    desc.name = "sandbox";
    desc.kind = env_.world_kind;
    desc.seed = env_.seed;
    desc.tick_rate = env_.tick_rate;
    world_ = std::make_unique<World>(*env_.registry, std::move(desc));
    const MutationGate gate = world_->OpenGate(GateReason::kLoad);
    if (auto loaded = LoadRequestedScene(*world_, PendingScene()); !loaded) {
      SE_LOG_ERROR("sandbox: {}", loaded.error().message);
    }
    world_->RecordHash();
    worlds_[0] = world_.get();
  }

  void OnExit() SE_REQUIRES(g_sim_owner) override {
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

std::unique_ptr<GameState> MakeSandbox(const StateEnv& env) {
  return std::make_unique<SandboxState>(env);
}

void RegisterStates(GameStateFactory& factory) {
  factory.Register(kSandboxState, &MakeSandbox);
}

// The legacy key bindings (config/config.json control-settings).
constexpr std::array<ActionBinding, 14> kBindings = {{
    {"up", "W"},
    {"left", "A"},
    {"down", "S"},
    {"right", "D"},
    {"jump", "Space"},
    {"exit", "Escape"},
    {"enter", "Enter"},
    {"back", "Enter"},
    {"a1", "U"},
    {"a2", "I"},
    {"a3", "O"},
    {"a4", "J"},
    {"a5", "K"},
    {"a6", "L"},
}};

}  // namespace

const GameModule& RampageModule() {
  static const GameModule kModule = {
      .name = "rampage",
      .register_components = &RegisterRampageComponents,
      .register_systems = &RegisterRampageSystems,
      .register_states = &RegisterStates,
      .start_state = kSandboxState,
      .actions = {.bindings = kBindings},
  };
  return kModule;
}

void SetSandboxScene(SceneRequest request) {
  PendingScene() = std::move(request);
}

const SceneRequest& SandboxScene() noexcept { return PendingScene(); }

std::expected<void, Error> LoadRequestedScene(World& world,
                                              const SceneRequest& request) {
  if (request.format == SceneRequest::Format::kLegacyScenario) {
    LegacyScenarioOptions options;
    options.root = request.legacy_root;
    options.rules = request.rules;
    auto loaded = LoadLegacyScenario(world, request.path, options);
    if (!loaded) return std::unexpected(loaded.error());
    return {};
  }
  SceneLoadOptions options;
  options.asset_root = request.asset_root;
  options.rules = request.rules;
  auto loaded = LoadSceneFile(world, request.path, options);
  if (!loaded) return std::unexpected(loaded.error());
  return {};
}

}  // namespace se::rampage
