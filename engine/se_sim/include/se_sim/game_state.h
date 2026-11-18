// Copyright 2026 The simple-engine-2 Authors.
//
// Game flow: a stack of GameStates (Title, Gameplay, Pause, GameOver), each
// owning its Worlds. GameStateStack keeps the deferred pushdown semantics of
// the legacy SceneManager (core/manager/SceneManager.cpp:40-69): push pauses
// the top, replace destroys it, pop resumes the state below. It adds a FIFO
// command queue, drops replace-by-default (every request names its op), and
// applies changes only at tick boundaries (ApplyPending, at the start of
// SimDriver::RunTick). So a GameOver requested in a frame's first tick takes
// effect before the second tick, which never runs in the old World (red
// team C6).
//
// Systems request changes with RequestGameState(world, ...), which queues
// the command in the World; ApplyPending collects those queues first.
//
// The content gate (M13): with a StateContentGate set, a push or replace
// whose state is not ready (its files loaded, its assets resident) stays
// at the front of the queue, with every command behind it, and Waiting()
// names it; the next ApplyPending asks again. The runtime holds the ticks
// while a state waits, so the command applies before the same tick as
// without a gate (replays, tests), and asset timing changes no tick.
//
// A state whose OnEnter fails (GameState::EnterError: its content did not
// load) is not kept: the stack exits and destroys it again, keeps the error
// (EnterError) and applies nothing more; the owner takes the fault path
// (M13a "a failed load of a state's text"; review fix F2, ADR-123).

#ifndef SE_SIM_GAME_STATE_H_
#define SE_SIM_GAME_STATE_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/hash.h"
#include "se_core/sim_clock.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/time.h"
#include "se_core/world.h"
#include "se_sim/content_source.h"

namespace se {

class ComponentRegistry;

enum class StackOp : std::uint8_t { kPush, kReplace, kPop };

struct StateCommand {
  StackOp op = StackOp::kPush;
  // The state to push or replace with; empty for a pop.
  std::string state_id;
  // Non-empty: the command applies only if this state is on top when it
  // reaches the front of the queue (the commands before it applied), and
  // is dropped otherwise. A focus loss's "push pause when gameplay is on
  // top" is decided where it applies, not when the event arrived (review
  // fix F6, ADR-124).
  std::string only_if_top;
};

// What a state needs to build its Worlds.
struct StateEnv {
  const ComponentRegistry* registry = nullptr;
  std::uint64_t seed = 0;
  std::uint32_t tick_rate = kDefaultTickRate;
  WorldKind world_kind = WorldKind::kRuntime;
  // Where the state's Worlds come from; null: the state reads nothing
  // (M2's tests) or reads files itself (the sandbox state before M13).
  const ContentSource* content = nullptr;
};

class GameState {
 public:
  virtual ~GameState() = default;

  // The state became the top of the stack: build its Worlds.
  virtual void OnEnter() SE_REQUIRES(g_sim_owner) = 0;
  // Another state was pushed over this one.
  virtual void OnPause() SE_REQUIRES(g_sim_owner) {}
  // The state above was popped.
  virtual void OnResume() SE_REQUIRES(g_sim_owner) {}
  // The state is about to be destroyed (popped or replaced).
  virtual void OnExit() SE_REQUIRES(g_sim_owner) {}

  // The Worlds the state owns, in a fixed order.
  [[nodiscard]] virtual std::span<World* const> Worlds() const = 0;
  // Whether the Worlds step while this state is on top.
  [[nodiscard]] virtual bool SimRunning() const { return true; }
  // Set when OnEnter could not build the state (its content did not load:
  // a parse error, an unknown component, a bad prefab): the stack then
  // takes the state off again and keeps the error (EnterError()). Null:
  // the state entered.
  [[nodiscard]] virtual const Error* EnterError() const noexcept {
    return nullptr;
  }
};

using StateCreator = std::unique_ptr<GameState> (*)(const StateEnv& env);

// The states a game module provides, by id.
class GameStateFactory {
 public:
  // |id| must be a string literal (the factory keeps the view).
  void Register(std::string_view id, StateCreator creator);
  [[nodiscard]] bool Contains(std::string_view id) const noexcept;
  // nullptr for an unknown id.
  [[nodiscard]] std::unique_ptr<GameState> Create(std::string_view id,
                                                  const StateEnv& env) const;

 private:
  std::vector<std::pair<std::string_view, StateCreator>> creators_;
};

// Asked before a push or replace creates its state (owner thread, at a
// tick boundary).
class StateContentGate {
 public:
  virtual ~StateContentGate() = default;
  // Whether the state |state_id| may be created now: its files loaded and
  // its assets resident.
  [[nodiscard]] virtual bool Ready(std::string_view state_id)
      SE_REQUIRES(g_sim_owner) = 0;
};

// A state on the stack (GameStateStack::Entries).
struct StackEntry {
  std::string_view id;
  const GameState* state = nullptr;
};

// A World to draw and its interpolation factor.
struct WorldRenderSource {
  const World* world = nullptr;
  float alpha = 1.0F;
};

class GameStateStack {
 public:
  GameStateStack(const GameStateFactory& factory, StateEnv env);
  GameStateStack(const GameStateStack&) = delete;
  GameStateStack& operator=(const GameStateStack&) = delete;
  ~GameStateStack();

  // Queues a command; it applies at the next tick boundary (|only_if_top|:
  // StateCommand::only_if_top).
  void Request(StackOp op, std::string_view state_id = {},
               std::string_view only_if_top = {});
  // Collects the commands systems queued in the Worlds, then applies every
  // queued command in FIFO order (including any queued by OnEnter).
  void ApplyPending() SE_REQUIRES(g_sim_owner);
  // Exits and destroys every state, top first.
  void Clear() SE_REQUIRES(g_sim_owner);

  // The top state's Worlds if it runs, else none.
  [[nodiscard]] std::vector<World*> RunningWorlds() const;
  // The top state's Worlds, running or not (SessionLog targets).
  [[nodiscard]] std::vector<World*> TopWorlds() const;
  // Every state's Worlds, bottom to top: the top at plan.alpha when it
  // runs, everything else at alpha 1.
  [[nodiscard]] std::vector<WorldRenderSource> RenderSources(
      const StepPlan& plan) const;
  [[nodiscard]] bool SimRunning() const;
  // The hash of every World on the stack, tagged with the state ids.
  [[nodiscard]] WorldHash Hash() const;

  // The states, bottom to top (the runtime keeps an AssetScope per state,
  // M9).
  [[nodiscard]] std::vector<StackEntry> Entries() const;

  [[nodiscard]] std::size_t Depth() const noexcept { return stack_.size(); }
  // Bumped by every change of the stack (a push, pop, replace or Clear):
  // the dedicated sim driver's snapshot generation follows it (M17a).
  [[nodiscard]] std::uint32_t Revision() const noexcept { return revision_; }
  [[nodiscard]] std::string_view TopId() const noexcept;
  [[nodiscard]] std::size_t PendingCount() const noexcept {
    return queue_.size();
  }
  // Conditional commands dropped because another state was on top.
  [[nodiscard]] std::uint32_t DroppedCount() const noexcept { return dropped_; }
  // Commands that named an unknown state, or popped an empty stack.
  [[nodiscard]] std::uint32_t RejectedCount() const noexcept {
    return rejected_;
  }
  // A state whose OnEnter failed (GameState::EnterError): it was exited and
  // destroyed again, and nothing more applies (ApplyPending keeps the queue)
  // until the owner, which takes the fault path (the game build: dump,
  // message box, quit; the editor: Play.Faulted), clears it. The error
  // keeps the content's path, line and column.
  [[nodiscard]] const Error* EnterError() const noexcept {
    return enter_error_ ? &*enter_error_ : nullptr;
  }
  // The state EnterError() is about.
  [[nodiscard]] std::string_view EnterErrorState() const noexcept {
    return enter_error_state_;
  }

  // A push or replace whose state |gate| does not report ready stays
  // queued, with every command behind it (FIFO); ApplyPending returns and
  // Waiting() names the state until a later boundary applies it. A null
  // gate (replays, tests) applies at once, so the same commands apply
  // before the same tick either way.
  void SetContentGate(StateContentGate* gate) noexcept { gate_ = gate; }
  // The state the queued push or replace at the front waits for (World
  // requests collected first); empty when nothing waits.
  [[nodiscard]] std::string_view Waiting() const noexcept;

 private:
  struct Entry {
    std::string id;
    std::unique_ptr<GameState> state;
  };

  void Apply(const StateCommand& command) SE_REQUIRES(g_sim_owner);
  void CollectWorldRequests() SE_REQUIRES(g_sim_owner);

  const GameStateFactory* factory_;
  StateEnv env_;
  std::vector<Entry> stack_;
  std::deque<StateCommand> queue_;
  std::uint32_t rejected_ = 0;
  std::uint32_t dropped_ = 0;
  std::uint32_t revision_ = 0;
  StateContentGate* gate_ = nullptr;
  // The front command waits at the gate (ApplyPending's last answer).
  bool waiting_ = false;
  std::optional<Error> enter_error_;
  std::string enter_error_state_;
};

// Queues a GameState command from a system; it applies at the next tick
// boundary.
void RequestGameState(World& world, StackOp op, std::string_view state_id = {})
    SE_REQUIRES(g_sim_owner);

}  // namespace se

#endif  // SE_SIM_GAME_STATE_H_
