// Copyright 2026 The simple-engine-2 Authors.
//
// FlowSystem and the flow's configuration (flow.h). The states are in
// flow_states.cc.

#include "game_rampage/flow.h"

#include <entt/entity/registry.hpp>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/module.h"
#include "game_rampage/scene.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/schedule.h"
#include "se_core/world.h"
#include "se_sim/content_source.h"
#include "se_sim/game_state.h"

namespace se::rampage {
namespace {

// Never destroyed (Google style: no non-trivial static destructors).
FlowConfig& PendingConfig() {
  static auto* const kConfig = new FlowConfig();
  return *kConfig;
}

bool Pressed(const InputFrame& input, Action action) {
  const auto id = static_cast<ActionId>(action);
  return id < kMaxActions && input.pressed[id] > 0;
}

StateCommand Replace(std::string_view state) {
  return StateCommand{StackOp::kReplace, std::string(state)};
}

}  // namespace

std::vector<StateCommand> FlowRules(FlowRole role, const InputFrame& input,
                                    bool player_alive) {
  switch (role) {
    case FlowRole::kNone:
      return {};
    case FlowRole::kTitle:
      if (Pressed(input, Action::kEnter)) return {Replace(kGameplayState)};
      if (Pressed(input, Action::kExit)) return {{StackOp::kPop, {}}};
      return {};
    case FlowRole::kGameplay:
      // The death is this tick's outcome: it wins over a pause asked for in
      // the same tick, which would freeze a lost game.
      if (!player_alive) return {Replace(kGameOverState)};
      if (Pressed(input, Action::kExit)) {
        return {{StackOp::kPush, std::string(kPauseState)}};
      }
      return {};
    case FlowRole::kPause:
      if (Pressed(input, Action::kExit)) return {{StackOp::kPop, {}}};
      // Decision 3 (until M19's menus): Jump returns to the Title. The pop
      // resumes Gameplay at the boundary, and the replace behind it in the
      // FIFO ends it before it ticks.
      if (Pressed(input, Action::kJump)) {
        return {{StackOp::kPop, {}}, Replace(kTitleState)};
      }
      return {};
    case FlowRole::kGameOver:
      if (Pressed(input, Action::kEnter)) return {Replace(kGameplayState)};
      if (Pressed(input, Action::kExit)) return {Replace(kTitleState)};
      return {};
  }
  return {};
}

bool PlayerAlive(const World& world) {
  const auto players =
      world.CReg().view<const PlayerControl>(entt::exclude<DestroyRequest>);
  return players.begin() != players.end();
}

void FlowSystem::Run(World& world, const TickContext& context) {
  const auto* flow = world.CReg().ctx().find<FlowContext>();
  if (flow == nullptr || flow->role == FlowRole::kNone) return;
  const bool alive = flow->role != FlowRole::kGameplay || PlayerAlive(world);
  for (const StateCommand& command :
       FlowRules(flow->role, context.input, alive)) {
    RequestGameState(world, command.op, command.state_id);
  }
}

void SetFlowConfig(FlowConfig config) { PendingConfig() = std::move(config); }

const FlowConfig& GetFlowConfig() { return PendingConfig(); }

std::string FlowSceneOf(std::string_view state_id) {
  if (state_id == kTitleState) return std::string(kTitleScene);
  if (state_id == kGameplayState) return PendingConfig().gameplay_scene;
  if (state_id == kPauseState) return std::string(kPauseScene);
  if (state_id == kGameOverState) return std::string(kGameOverScene);
  return {};
}

std::expected<std::string, Error> ReadContent(const ContentSource* content,
                                              std::string_view content_root,
                                              std::string_view path) {
  if (content != nullptr) {
    auto text = content->Text(path);
    if (!text) {
      Error error = text.error();
      if (error.path.empty()) error.path = std::string(path);
      return std::unexpected(std::move(error));
    }
    return std::string(*text);
  }
  std::string file = std::string(content_root);
  if (!file.empty()) file += '/';
  file += path;
  return ReadTextFile(file);
}

}  // namespace se::rampage
