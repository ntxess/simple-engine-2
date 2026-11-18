// Copyright 2026 The simple-engine-2 Authors.
//
// GameStateStack: the pushdown semantics of the legacy SceneManager
// (push pauses, replace destroys, pop resumes), a FIFO queue, explicit ops,
// changes only at tick boundaries, and the red team's C6 case.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/schedule.h"
#include "se_core/thread_role.h"
#include "se_core/time.h"
#include "se_core/world.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"
#include "support/kernel_session.h"
#include "support/kernel_test_module.h"
#include "support/test_roles.h"

namespace se {
namespace {

std::vector<std::string>& Trace() {
  static std::vector<std::string> trace;
  return trace;
}

const ComponentRegistry& CoreRegistry() {
  static const ComponentRegistry kRegistry = [] {
    ComponentRegistry r;
    RegisterCoreComponents(r);
    r.Seal();
    return r;
  }();
  return kRegistry;
}

// Records its callbacks; owns one World; "pause" states do not run.
class TraceState final : public GameState {
 public:
  TraceState(std::string name, bool runs, WorldKind kind = WorldKind::kRuntime)
      : name_(std::move(name)), runs_(runs), kind_(kind) {}

  void OnEnter() SE_REQUIRES(g_sim_owner) override {
    world_ = std::make_unique<World>(CoreRegistry(), WorldDesc{.kind = kind_});
    worlds_[0] = world_.get();
    Trace().push_back(name_ + ".enter");
  }
  void OnPause() SE_REQUIRES(g_sim_owner) override {
    Trace().push_back(name_ + ".pause");
  }
  void OnResume() SE_REQUIRES(g_sim_owner) override {
    Trace().push_back(name_ + ".resume");
  }
  void OnExit() SE_REQUIRES(g_sim_owner) override {
    Trace().push_back(name_ + ".exit");
  }
  [[nodiscard]] std::span<World* const> Worlds() const override {
    return worlds_;
  }
  [[nodiscard]] bool SimRunning() const override { return runs_; }

 private:
  std::string name_;
  bool runs_;
  WorldKind kind_;
  std::unique_ptr<World> world_;
  std::array<World*, 1> worlds_{};
};

// A state whose content does not load: OnEnter sets EnterError.
class BrokenState final : public GameState {
 public:
  void OnEnter() SE_REQUIRES(g_sim_owner) override {
    Trace().emplace_back("broken.enter");
    error_ = Error{ErrorCode::kParse, "unknown component", "broken.json", 3, 7};
  }
  void OnExit() SE_REQUIRES(g_sim_owner) override {
    Trace().emplace_back("broken.exit");
  }
  [[nodiscard]] std::span<World* const> Worlds() const override { return {}; }
  [[nodiscard]] const Error* EnterError() const noexcept override {
    return error_ ? &*error_ : nullptr;
  }

 private:
  std::optional<Error> error_;
};

GameStateFactory TraceFactory() {
  GameStateFactory factory;
  factory.Register("a", [](const StateEnv&) -> std::unique_ptr<GameState> {
    return std::make_unique<TraceState>("a", true);
  });
  factory.Register("b", [](const StateEnv&) -> std::unique_ptr<GameState> {
    return std::make_unique<TraceState>("b", true);
  });
  factory.Register("pause", [](const StateEnv&) -> std::unique_ptr<GameState> {
    return std::make_unique<TraceState>("pause", false);
  });
  factory.Register("play", [](const StateEnv&) -> std::unique_ptr<GameState> {
    return std::make_unique<TraceState>("play", true, WorldKind::kPlay);
  });
  factory.Register("broken", [](const StateEnv&) -> std::unique_ptr<GameState> {
    return std::make_unique<BrokenState>();
  });
  return factory;
}

TEST_CASE("Push pauses, replace destroys, pop resumes", "[game_state]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  Trace().clear();
  const GameStateFactory factory = TraceFactory();
  GameStateStack stack(factory, {});
  stack.Request(StackOp::kPush, "a");
  REQUIRE(stack.Depth() == 0);  // nothing happens before the boundary
  stack.ApplyPending();
  REQUIRE(stack.TopId() == "a");
  stack.Request(StackOp::kPush, "b");
  stack.ApplyPending();
  REQUIRE(stack.Depth() == 2);
  stack.Request(StackOp::kReplace, "pause");
  stack.ApplyPending();
  REQUIRE(stack.Depth() == 2);
  REQUIRE(stack.TopId() == "pause");
  stack.Request(StackOp::kPop);
  stack.ApplyPending();
  REQUIRE(stack.TopId() == "a");
  CHECK(Trace() == std::vector<std::string>{"a.enter", "a.pause", "b.enter",
                                            "b.exit", "pause.enter",
                                            "pause.exit", "a.resume"});
  stack.Clear();
  REQUIRE(stack.Depth() == 0);
  REQUIRE(Trace().back() == "a.exit");
}

TEST_CASE("Commands apply in FIFO order at one boundary", "[game_state]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  Trace().clear();
  const GameStateFactory factory = TraceFactory();
  GameStateStack stack(factory, {});
  stack.Request(StackOp::kPush, "a");
  stack.Request(StackOp::kPush, "b");
  stack.Request(StackOp::kPop);
  stack.Request(StackOp::kPush, "pause");
  REQUIRE(stack.PendingCount() == 4);
  stack.ApplyPending();
  REQUIRE(stack.PendingCount() == 0);
  REQUIRE(stack.Depth() == 2);
  REQUIRE(stack.TopId() == "pause");
  CHECK(Trace() == std::vector<std::string>{"a.enter", "a.pause", "b.enter",
                                            "b.exit", "a.resume", "a.pause",
                                            "pause.enter"});
  stack.Clear();
}

TEST_CASE("Unknown states and pops of an empty stack are rejected",
          "[game_state]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  const GameStateFactory factory = TraceFactory();
  GameStateStack stack(factory, {});
  stack.Request(StackOp::kPop);
  stack.Request(StackOp::kPush, "nope");
  stack.ApplyPending();
  REQUIRE(stack.RejectedCount() == 2);
  REQUIRE(stack.Depth() == 0);
}

TEST_CASE("Only a running top state steps; render alphas", "[game_state]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  const GameStateFactory factory = TraceFactory();
  GameStateStack stack(factory, {});
  CHECK_FALSE(stack.SimRunning());
  CHECK(stack.RunningWorlds().empty());
  stack.Request(StackOp::kPush, "a");
  stack.ApplyPending();
  CHECK(stack.SimRunning());
  REQUIRE(stack.RunningWorlds().size() == 1);
  StepPlan plan;
  plan.alpha = 0.25F;
  auto sources = stack.RenderSources(plan);
  REQUIRE(sources.size() == 1);
  CHECK(sources[0].alpha == 0.25F);

  stack.Request(StackOp::kPush, "pause");
  stack.ApplyPending();
  CHECK_FALSE(stack.SimRunning());
  CHECK(stack.RunningWorlds().empty());
  REQUIRE(stack.TopWorlds().size() == 1);
  sources = stack.RenderSources(plan);
  REQUIRE(sources.size() == 2);
  CHECK(sources[0].alpha == 1.0F);  // paused worlds render at alpha 1
  CHECK(sources[1].alpha == 1.0F);
  stack.Clear();
}

TEST_CASE("Systems request state changes for the next boundary",
          "[game_state]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  Trace().clear();
  const GameStateFactory factory = TraceFactory();
  GameStateStack stack(factory, {});
  stack.Request(StackOp::kPush, "a");
  stack.ApplyPending();
  World& world = *stack.RunningWorlds().front();
  RequestGameState(world, StackOp::kReplace, "b");
  REQUIRE(stack.TopId() == "a");
  stack.ApplyPending();
  REQUIRE(stack.TopId() == "b");
  CHECK(Trace() == std::vector<std::string>{"a.enter", "a.exit", "b.enter"});
  stack.Clear();
}

// A Play World mutates only through its gate. Taking the commands its
// systems queued happens at the boundary, outside the tick's gate.
TEST_CASE("Play Worlds hand their GameState requests over at the boundary",
          "[game_state]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  Trace().clear();
  const GameStateFactory factory = TraceFactory();
  GameStateStack stack(factory, {});
  stack.Request(StackOp::kPush, "play");
  stack.ApplyPending();
  World& world = *stack.RunningWorlds().front();
  REQUIRE(world.Desc().kind == WorldKind::kPlay);
  {
    // What Schedule::Step does around the systems.
    const MutationGate gate = world.OpenGate(GateReason::kTick);
    RequestGameState(world, StackOp::kPush, "b");
  }
  REQUIRE_FALSE(world.GateOpen());
  stack.ApplyPending();
  REQUIRE(stack.TopId() == "b");
  stack.Request(StackOp::kPop);
  stack.ApplyPending();
  REQUIRE(stack.TopId() == "play");
  // Boundaries with nothing queued do not touch the World.
  stack.ApplyPending();
  CHECK_FALSE(world.GateOpen());
  CHECK(Trace() == std::vector<std::string>{"play.enter", "play.pause",
                                            "b.enter", "b.exit",
                                            "play.resume"});
  stack.Clear();
}

// The kernel module's GameOver rule in a Play World: tick 300 requests it,
// tick 301 runs in the game_over World, with the same hashes as a runtime
// World (and through --det-crosscheck, which clones the Play World).
TEST_CASE("The kernel module's GameOver works in a Play World",
          "[game_state][c6]") {
  const testing::MainThreadScope main_thread;
  testing::KernelSessionOptions options;
  options.ticks = testing::kGameOverTick + 20;
  options.edits_path = testing::EditsFixturePath();
  const testing::KernelSessionResult runtime =
      testing::RunKernelSession(options);
  REQUIRE(runtime.error.empty());
  options.world_kind = WorldKind::kPlay;
  options.det_crosscheck = 7;
  const testing::KernelSessionResult play = testing::RunKernelSession(options);
  REQUIRE(play.error.empty());
  REQUIRE(play.hashes.size() == options.ticks);
  CHECK(play.top_states[testing::kGameOverTick - 1] == testing::kGameplayState);
  CHECK(play.top_states[testing::kGameOverTick] == testing::kGameOverState);
  REQUIRE(play.session_applied == 4);
  REQUIRE(play.session_failures == 0);
  CHECK(play.crosscheck_ok);
  REQUIRE(play.crosschecks > 0);
  CHECK(play.hashes == runtime.hashes);
}

// Red team C6: the GameOver requested by a rule in the first tick of a
// two-tick frame must not let the second tick run in the old World.
TEST_CASE(
    "A GameOver in a frame's first tick replaces the World before the "
    "second tick (C6)",
    "[game_state][c6]") {
  testing::MainThreadScope main_thread;
  ModuleInstance module(testing::KernelTestModule());
  StateEnv env;
  env.registry = &module.Registry();
  env.seed = 42;
  GameStateStack states(module.Factory(), env);
  {
    const InlineOwnerScope owner(OnMainThread());
    states.Request(StackOp::kPush, testing::kGameplayState);
    states.ApplyPending();
  }
  jobs::ForkJoinPool pool;
  SimDriver driver(module.MutableSchedule(), pool);
  SessionLog log;
  ReplayRecorder recorder;
  NullAudioSink audio;
  testing::ScriptedInput input(42);

  // Single ticks up to tick 299, then one 60 Hz frame of two ticks:
  // 300 (the rule fires) and 301.
  StepPlan one;
  one.steps = 1;
  while (driver.NextTick() < testing::kGameOverTick) {
    driver.TickInline(states, one, input, log, recorder, audio);
  }
  StepPlan two;
  two.steps = 2;
  driver.TickInline(states, two, input, log, recorder, audio);
  const auto results = driver.FrameResults();
  REQUIRE(results.size() == 2);
  CHECK(results[0].tick == testing::kGameOverTick);
  CHECK(results[0].top_state == testing::kGameplayState);
  CHECK(results[1].tick == testing::kGameOverTick + 1);
  CHECK(results[1].top_state == testing::kGameOverState);
  REQUIRE(results[1].worlds_stepped == 1);
  // The new World stepped once: its clock is at 1, not at 301.
  REQUIRE(states.RunningWorlds().size() == 1);
  REQUIRE(states.RunningWorlds().front()->Clock().tick == 1);
  const InlineOwnerScope owner(OnMainThread());
  states.Clear();
}

// Review fix F2: a state whose OnEnter fails is taken off again, its error
// kept with its path, line and column, and nothing more applies.
TEST_CASE("A state that cannot enter is not kept", "[game_state]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  Trace().clear();
  const GameStateFactory factory = TraceFactory();
  GameStateStack stack(factory, {});
  stack.Request(StackOp::kPush, "a");
  stack.Request(StackOp::kPush, "broken");
  stack.Request(StackOp::kPush, "b");
  stack.ApplyPending();
  REQUIRE(stack.EnterError() != nullptr);
  REQUIRE(stack.EnterErrorState() == "broken");
  REQUIRE(stack.EnterError()->path == "broken.json");
  REQUIRE(stack.EnterError()->line == 3);
  REQUIRE(stack.EnterError()->column == 7);
  REQUIRE(stack.Depth() == 1);
  REQUIRE(stack.TopId() == "a");
  // "b" stays queued: the owner takes the fault path.
  REQUIRE(stack.PendingCount() == 1);
  stack.ApplyPending();
  REQUIRE(stack.Depth() == 1);
  CHECK(Trace() == std::vector<std::string>{"a.enter", "a.pause",
                                            "broken.enter", "broken.exit",
                                            "a.resume"});
  stack.Clear();
  REQUIRE(stack.EnterError() == nullptr);
}

// Review fix F6: a command conditional on the top state is decided where it
// applies, after the commands before it, so a World's own request of the
// same boundary (Escape's Pause, a death's GameOver) wins over a focus
// loss's pause; through the SessionLog, a replay decides the same.
TEST_CASE("A conditional command applies only over its state", "[game_state]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  const GameStateFactory factory = TraceFactory();
  GameStateStack stack(factory, {});
  stack.Request(StackOp::kPush, "a");
  stack.ApplyPending();
  // The World's request first, then the focus loss's "pause if 'a'".
  stack.Request(StackOp::kPush, "pause");
  stack.Request(StackOp::kPush, "pause", "a");
  stack.ApplyPending();
  REQUIRE(stack.Depth() == 2);
  REQUIRE(stack.DroppedCount() == 1);
  // Over 'a' it applies.
  stack.Request(StackOp::kPop);
  stack.Request(StackOp::kPush, "pause", "a");
  stack.ApplyPending();
  REQUIRE(stack.Depth() == 2);
  REQUIRE(stack.TopId() == "pause");
  REQUIRE(stack.DroppedCount() == 1);
  stack.Clear();

  // The same through a SessionLog entry, written and read back.
  stack.Request(StackOp::kPush, "a");
  stack.ApplyPending();
  SessionLog live;
  live.Enqueue(MakeGameStateCommand(2, StackOp::kPush, "pause", "a"), 2, stack);
  auto read = SessionLog::Read(live.Write());
  REQUIRE(read.has_value());
  stack.Request(StackOp::kReplace, "b");
  stack.ApplyPending();
  read->ApplyDue(2, stack);
  REQUIRE(read->Failures() == 0);
  REQUIRE(stack.TopId() == "b");
  REQUIRE(stack.Depth() == 1);
  REQUIRE(stack.DroppedCount() == 2);
  stack.Clear();
}

}  // namespace
}  // namespace se
