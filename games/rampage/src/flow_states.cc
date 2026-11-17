// Copyright 2026 The simple-engine-2 Authors.
//
// The Title, Gameplay, Pause and GameOver states (flow.h). Each owns one
// World built from its scene (flow.h's FlowSceneOf) through
// StateEnv::content, with prefabs resolved through the same source, and
// carries a FlowContext with its role. Pause is an overlay: pushed over
// Gameplay, its World steps and Gameplay's does not, and both draw.

#include <array>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "game_rampage/components.h"
#include "game_rampage/flow.h"
#include "game_rampage/scene.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/world.h"
#include "se_sim/audio_events.h"
#include "se_sim/game_state.h"

namespace se::rampage {
namespace {

// Loads |scene| into the empty |world| through |env|'s content (prefabs
// too), else from the configured content root.
std::expected<SceneSummary, Error> LoadFlowScene(World& world,
                                                 const StateEnv& env,
                                                 const std::string& scene)
    SE_REQUIRES(g_sim_owner) {
  const FlowConfig& config = GetFlowConfig();
  const ContentSource* content = env.content;
  std::string root = config.content_root;
  auto text = ReadContent(content, root, scene);
  if (!text) return std::unexpected(text.error());
  SceneLoadOptions options;
  options.asset_root = root.empty()
                           ? std::string(kContentAssetDir)
                           : root + "/" + std::string(kContentAssetDir);
  options.rules = config.rules;
  options.resolver = [content, root](std::string_view ref) {
    std::string path(kContentAssetDir);
    path += '/';
    path += ref;
    return ReadContent(content, root, path);
  };
  auto loaded = LoadScene(world, *text, options);
  if (!loaded) {
    Error error = loaded.error();
    error.message = scene + ": " + error.message;
    if (error.path.empty()) error.path = scene;
    return std::unexpected(std::move(error));
  }
  return loaded;
}

class FlowState final : public GameState {
 public:
  FlowState(const StateEnv& env, std::string_view id, FlowRole role)
      : env_(env), id_(id), role_(role) {}

  void OnEnter() SE_REQUIRES(g_sim_owner) override {
    WorldDesc desc;
    desc.name = std::string(id_);
    desc.kind = env_.world_kind;
    desc.seed = env_.seed;
    desc.tick_rate = env_.tick_rate;
    world_ = std::make_unique<World>(*env_.registry, std::move(desc));
    const MutationGate gate = world_->OpenGate(GateReason::kLoad);
    const std::string scene = FlowSceneOf(id_);
    auto loaded = LoadFlowScene(*world_, env_, scene);
    if (loaded) {
      SE_LOG_INFO("flow: entered '{}' ({}: {} entities)", id_, scene,
                  loaded->entities);
    } else {
      const Error& error = loaded.error();
      SE_LOG_ERROR("flow: '{}' cannot load {} (line {}, column {}): {}", id_,
                   error.path, error.line, error.column, error.message);
    }
    world_->Reg().ctx().emplace<FlowContext>(FlowContext{role_});
    // The rules' sounds (M8); never part of the World's state.
    if (role_ == FlowRole::kGameplay) AddAudioEvents(*world_);
    world_->RecordHash();
    worlds_[0] = world_.get();
  }

  void OnResume() SE_REQUIRES(g_sim_owner) override {
    SE_LOG_INFO("flow: resumed '{}'", id_);
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
  std::string_view id_;
  FlowRole role_;
  std::unique_ptr<World> world_;
  std::array<World*, 1> worlds_{};
};

std::unique_ptr<GameState> MakeTitle(const StateEnv& env) {
  return std::make_unique<FlowState>(env, kTitleState, FlowRole::kTitle);
}

std::unique_ptr<GameState> MakeGameplay(const StateEnv& env) {
  return std::make_unique<FlowState>(env, kGameplayState, FlowRole::kGameplay);
}

std::unique_ptr<GameState> MakePause(const StateEnv& env) {
  return std::make_unique<FlowState>(env, kPauseState, FlowRole::kPause);
}

std::unique_ptr<GameState> MakeGameOver(const StateEnv& env) {
  return std::make_unique<FlowState>(env, kGameOverState, FlowRole::kGameOver);
}

}  // namespace

void RegisterFlowStates(GameStateFactory& factory) {
  factory.Register(kTitleState, &MakeTitle);
  factory.Register(kGameplayState, &MakeGameplay);
  factory.Register(kPauseState, &MakePause);
  factory.Register(kGameOverState, &MakeGameOver);
}

}  // namespace se::rampage
