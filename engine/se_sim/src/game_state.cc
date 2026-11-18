// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/game_state.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/assert.h"
#include "se_core/hash.h"
#include "se_core/log.h"
#include "se_core/world.h"

namespace se {
namespace {

// The commands a World's systems queued during a tick (registry context).
struct WorldStateRequests {
  std::vector<StateCommand> commands;
};

// Bounds the commands applied in one ApplyPending (a state whose OnEnter
// pushes itself would otherwise never stop).
constexpr int kMaxCommandsPerBoundary = 64;

}  // namespace

void RequestGameState(World& world, StackOp op, std::string_view state_id) {
  auto& ctx = world.Reg().ctx();
  if (!ctx.contains<WorldStateRequests>()) ctx.emplace<WorldStateRequests>();
  ctx.get<WorldStateRequests>().commands.push_back(
      StateCommand{op, std::string(state_id)});
}

void GameStateFactory::Register(std::string_view id, StateCreator creator) {
  SE_ASSERT_MSG(!Contains(id), "GameState id registered twice");
  SE_ASSERT_MSG(creator != nullptr, "a GameState needs a creator");
  creators_.emplace_back(id, creator);
}

bool GameStateFactory::Contains(std::string_view id) const noexcept {
  return std::any_of(creators_.begin(), creators_.end(),
                     [id](const auto& entry) { return entry.first == id; });
}

std::unique_ptr<GameState> GameStateFactory::Create(std::string_view id,
                                                    const StateEnv& env) const {
  for (const auto& [name, creator] : creators_) {
    if (name == id) return creator(env);
  }
  return nullptr;
}

GameStateStack::GameStateStack(const GameStateFactory& factory, StateEnv env)
    : factory_(&factory), env_(env) {}

GameStateStack::~GameStateStack() = default;

void GameStateStack::Request(StackOp op, std::string_view state_id,
                             std::string_view only_if_top) {
  queue_.push_back(
      StateCommand{op, std::string(state_id), std::string(only_if_top)});
}

void GameStateStack::CollectWorldRequests() {
  for (const Entry& entry : stack_) {
    for (World* world : entry.state->Worlds()) {
      const auto* queued = world->CReg().ctx().find<WorldStateRequests>();
      if (queued == nullptr || queued->commands.empty()) continue;
      // Taking the queue changes the World's registry context, so a Play
      // World needs its gate open, as for any other boundary mutation.
      const MutationGate gate = world->OpenGate(GateReason::kStateChange);
      auto& requests = world->Reg().ctx().get<WorldStateRequests>();
      for (StateCommand& command : requests.commands) {
        queue_.push_back(std::move(command));
      }
      requests.commands.clear();
    }
  }
}

void GameStateStack::ApplyPending() {
  // A state that failed to enter: its owner takes the fault path.
  if (enter_error_) return;
  CollectWorldRequests();
  const bool was_waiting = waiting_;
  waiting_ = false;
  for (int applied = 0; !queue_.empty(); ++applied) {
    if (applied == kMaxCommandsPerBoundary) {
      SE_LOG_ERROR(
          "more than {} GameState commands at one tick boundary; "
          "{} left for the next",
          kMaxCommandsPerBoundary, queue_.size());
      break;
    }
    const StateCommand& front = queue_.front();
    // A conditional command whose state is no longer on top is dropped
    // here, where a replay drops it too (before the gate is asked).
    if (!front.only_if_top.empty() && TopId() != front.only_if_top) {
      ++dropped_;
      SE_LOG_INFO(
          "GameState command for '{}' dropped: '{}' is on top, not '{}'",
          front.state_id, TopId(), front.only_if_top);
      queue_.pop_front();
      continue;
    }
    // The content gate: a state is created only once it is ready; the
    // command and every one behind it wait (unknown ids are rejected by
    // Apply as before).
    if (gate_ != nullptr && front.op != StackOp::kPop &&
        factory_->Contains(front.state_id) && !gate_->Ready(front.state_id)) {
      waiting_ = true;
      if (!was_waiting) {
        SE_LOG_DEBUG("GameState '{}' waits at the content gate ({} queued)",
                     front.state_id, queue_.size());
      }
      return;
    }
    const StateCommand command = std::move(queue_.front());
    queue_.pop_front();
    Apply(command);
    if (enter_error_) return;
  }
}

void GameStateStack::Apply(const StateCommand& command) {
  if (command.op == StackOp::kPop) {
    if (stack_.empty()) {
      ++rejected_;
      SE_LOG_WARN("GameState pop on an empty stack ignored");
      return;
    }
    stack_.back().state->OnExit();
    stack_.pop_back();
    ++revision_;
    if (!stack_.empty()) stack_.back().state->OnResume();
    return;
  }
  std::unique_ptr<GameState> state = factory_->Create(command.state_id, env_);
  if (state == nullptr) {
    ++rejected_;
    SE_LOG_ERROR("unknown GameState '{}'", command.state_id);
    return;
  }
  if (!stack_.empty()) {
    if (command.op == StackOp::kReplace) {
      stack_.back().state->OnExit();
      stack_.pop_back();
    } else {
      stack_.back().state->OnPause();
    }
  }
  stack_.push_back(Entry{command.state_id, std::move(state)});
  ++revision_;
  stack_.back().state->OnEnter();
  if (const Error* error = stack_.back().state->EnterError()) {
    // Not created after all: the state goes again, and the error stays.
    enter_error_ = *error;
    enter_error_state_ = command.state_id;
    SE_LOG_ERROR("GameState '{}' could not enter: {} ({}:{}:{})",
                 command.state_id, error->message, error->path, error->line,
                 error->column);
    stack_.back().state->OnExit();
    stack_.pop_back();
    if (command.op == StackOp::kPush && !stack_.empty()) {
      stack_.back().state->OnResume();
    }
  }
}

void GameStateStack::Clear() {
  queue_.clear();
  enter_error_.reset();
  enter_error_state_.clear();
  waiting_ = false;
  if (!stack_.empty()) ++revision_;
  while (!stack_.empty()) {
    stack_.back().state->OnExit();
    stack_.pop_back();
  }
}

std::vector<World*> GameStateStack::RunningWorlds() const {
  if (!SimRunning()) return {};
  return TopWorlds();
}

std::vector<World*> GameStateStack::TopWorlds() const {
  if (stack_.empty()) return {};
  const std::span<World* const> worlds = stack_.back().state->Worlds();
  return {worlds.begin(), worlds.end()};
}

std::vector<WorldRenderSource> GameStateStack::RenderSources(
    const StepPlan& plan) const {
  std::vector<WorldRenderSource> sources;
  const bool running = SimRunning();
  for (std::size_t i = 0; i < stack_.size(); ++i) {
    const bool top = i + 1 == stack_.size();
    const float alpha = top && running ? plan.alpha : 1.0F;
    for (const World* world : stack_[i].state->Worlds()) {
      sources.push_back(WorldRenderSource{world, alpha});
    }
  }
  return sources;
}

bool GameStateStack::SimRunning() const {
  return !stack_.empty() && stack_.back().state->SimRunning();
}

WorldHash GameStateStack::Hash() const {
  WorldHash combined;
  for (std::size_t s = 0; s < stack_.size(); ++s) {
    const std::span<World* const> worlds = stack_[s].state->Worlds();
    for (std::size_t w = 0; w < worlds.size(); ++w) {
      const World& world = *worlds[w];
      const WorldHash hash =
          world.HashDirty() ? world.Hash() : world.LastHash();
      Hasher tag;
      tag.AddString(stack_[s].id);
      tag.AddU64(s);
      tag.AddU64(w);
      combined.sum += Mix64(hash.Folded() ^ tag.Finish());
      combined.xr ^= Mix64(hash.Folded() + tag.Finish());
      combined.count += hash.count;
      combined.tick = std::max(combined.tick, hash.tick);
    }
  }
  return combined;
}

std::vector<StackEntry> GameStateStack::Entries() const {
  std::vector<StackEntry> entries;
  entries.reserve(stack_.size());
  for (const Entry& entry : stack_) {
    entries.push_back({entry.id, entry.state.get()});
  }
  return entries;
}

std::string_view GameStateStack::Waiting() const noexcept {
  if (!waiting_ || queue_.empty()) return {};
  return queue_.front().state_id;
}

std::string_view GameStateStack::TopId() const noexcept {
  return stack_.empty() ? std::string_view() : stack_.back().id;
}

}  // namespace se
