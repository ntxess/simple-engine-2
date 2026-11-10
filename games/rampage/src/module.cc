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
#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_sim/audio_events.h"
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
    // The rules' sounds (M8); never part of the World's state.
    AddAudioEvents(*world_);
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

// game_rampage's components, then the renderer's it draws with
// (SpriteRenderer, and Camera2D for scenes that place a camera). Neither
// render component is hashed, so they do not change the world hash.
void RegisterComponents(ComponentRegistry& registry) {
  RegisterRampageComponents(registry);
  RegisterRenderComponents(registry);
}

std::unique_ptr<GameState> MakeSandbox(const StateEnv& env) {
  return std::make_unique<SandboxState>(env);
}

void RegisterStates(GameStateFactory& factory) {
  factory.Register(kSandboxState, &MakeSandbox);
}

// The legacy key bindings (config/config.json control-settings, whose
// entries rebind them) and the default gamepad bindings (ADR-074): the
// d-pad and the left stick move, the face buttons, shoulders and stick
// clicks are the discrete actions.
constexpr std::array<ActionBinding, 14> kBindings = {{
    {"up", "w", "key-up", "dpup,-lefty"},
    {"left", "a", "key-left", "dpleft,-leftx"},
    {"down", "s", "key-down", "dpdown,+lefty"},
    {"right", "d", "key-right", "dpright,+leftx"},
    {"jump", "space", "key-jump", "south"},
    {"exit", "esc", "key-exit", "back"},
    {"enter", "enter", "key-enter", "start"},
    {"back", "enter", "key-back", "east"},
    {"a1", "u", "key-1", "west"},
    {"a2", "i", "key-2", "north"},
    {"a3", "o", "key-3", "leftshoulder"},
    {"a4", "j", "key-4", "rightshoulder"},
    {"a5", "k", "key-5", "leftstick"},
    {"a6", "l", "key-6", "rightstick"},
}};

}  // namespace

const GameModule& RampageModule() {
  static const GameModule kModule = {
      .name = "rampage",
      .register_components = &RegisterComponents,
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
