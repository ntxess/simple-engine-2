// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/session_log.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/sim_driver.h"
#include "support/kernel_test_module.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::KernelTestModule;
using testing::MoverGuid;

// A kernel-module stack with the gameplay state on top.
struct Fixture {
  explicit Fixture(WorldKind kind = WorldKind::kRuntime)
      : module(KernelTestModule()) {
    StateEnv env;
    env.registry = &module.Registry();
    env.seed = 3;
    env.world_kind = kind;
    states = std::make_unique<GameStateStack>(module.Factory(), env);
  }
  void Start() const SE_REQUIRES(g_sim_owner) {
    states->Request(StackOp::kPush, testing::kGameplayState);
    states->ApplyPending();
  }
  [[nodiscard]] World& Top() const { return *states->TopWorlds().front(); }

  ModuleInstance module;
  std::unique_ptr<GameStateStack> states;
};

SessionEntry MoverEdit(const ComponentRegistry& registry, Tick tick,
                       float radius) {
  return MakeComponentEntry(
             registry, SessionOp::kSetComponent, tick, MoverGuid(1), "Mover",
             R"({"center":{"x":1,"y":2},"radius":)" + std::to_string(radius) +
                 R"(,"angular_speed":0,"phase":0})")
      .value();
}

float MoverRadius(const World& world) {
  return world.CReg().get<testing::Mover>(*world.Find(MoverGuid(1))).radius;
}

TEST_CASE("Entries apply at the boundary before their tick", "[session_log]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner;
  Fixture f;
  f.Start();
  SessionLog log;
  log.Enqueue(MoverEdit(f.module.Registry(), 10, 77.0F), 1, *f.states);
  REQUIRE(log.PendingCount() == 1);
  log.ApplyDue(9, *f.states);
  CHECK(log.Applied().empty());
  CHECK(MoverRadius(f.Top()) != 77.0F);
  log.ApplyDue(10, *f.states);
  REQUIRE(log.Applied().size() == 1);
  CHECK(MoverRadius(f.Top()) == 77.0F);
  REQUIRE(log.Failures() == 0);
  f.states->Clear();
}

TEST_CASE("Entries apply in (tick, seq) order", "[session_log]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner;
  Fixture f;
  f.Start();
  SessionLog log;
  log.AddScheduled(MoverEdit(f.module.Registry(), 20, 2.0F));
  log.AddScheduled(MoverEdit(f.module.Registry(), 10, 1.0F));
  log.AddScheduled(MoverEdit(f.module.Registry(), 20, 3.0F));
  log.ApplyDue(100, *f.states);
  REQUIRE(log.Applied().size() == 3);
  REQUIRE(log.Applied()[0].apply_before == 10);
  CHECK(log.Applied()[1].seq < log.Applied()[2].seq);
  CHECK(MoverRadius(f.Top()) == 3.0F);  // the later of the two at tick 20
  f.states->Clear();
}

TEST_CASE("A live entry for a past tick is logged against the next tick",
          "[session_log]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner;
  Fixture f;
  f.Start();
  SessionLog log;
  // Queued at tick 11 for tick 3, which has already run.
  log.Enqueue(MoverEdit(f.module.Registry(), 3, 5.0F), 11, *f.states);
  log.ApplyDue(10, *f.states);
  CHECK(log.Applied().empty());
  log.ApplyDue(11, *f.states);
  REQUIRE(log.Applied().size() == 1);
  REQUIRE(log.Applied()[0].apply_before == 11);
  CHECK(MoverRadius(f.Top()) == 5.0F);
  f.states->Clear();
}

// The editor's paused Play: the gameplay state with its World, not stepping
// while g_play_running is false.
bool g_play_running = true;

const GameStateFactory& KernelStates() {
  static const GameStateFactory kStates = [] {
    GameStateFactory factory;
    KernelTestModule().register_states(factory);
    return factory;
  }();
  return kStates;
}

class PausablePlay final : public GameState {
 public:
  explicit PausablePlay(const StateEnv& env)
      : inner_(KernelStates().Create(testing::kGameplayState, env)) {}
  void OnEnter() SE_REQUIRES(g_sim_owner) override { inner_->OnEnter(); }
  void OnExit() SE_REQUIRES(g_sim_owner) override { inner_->OnExit(); }
  [[nodiscard]] std::span<World* const> Worlds() const override {
    return inner_->Worlds();
  }
  [[nodiscard]] bool SimRunning() const override { return g_play_running; }

 private:
  std::unique_ptr<GameState> inner_;
};

constexpr std::string_view kPlayState = "play";

const GameStateFactory& PlayStates() {
  static const GameStateFactory kStates = [] {
    GameStateFactory factory;
    factory.Register(kPlayState,
                     [](const StateEnv& env) -> std::unique_ptr<GameState> {
                       return std::make_unique<PausablePlay>(env);
                     });
    return factory;
  }();
  return kStates;
}

// A Play session driven tick by tick through SimDriver::RunTick.
class PlaySession {
 public:
  PlaySession()
      : module_(KernelTestModule()), driver_(module_.MutableSchedule(), pool_) {
    StateEnv env;
    env.registry = &module_.Registry();
    env.seed = 3;
    env.world_kind = WorldKind::kPlay;
    states_ = std::make_unique<GameStateStack>(PlayStates(), env);
  }

  void Start() SE_REQUIRES(g_sim_owner) {
    states_->Request(StackOp::kPush, kPlayState);
    states_->ApplyPending();
  }
  // Runs ticks up to and including |last|.
  void RunTo(Tick last, SessionLog& log) SE_REQUIRES(g_sim_owner) {
    while (driver_.NextTick() <= last) {
      const InputFrame frame =
          testing::ScriptedInput::FrameFor(driver_.NextTick(), 3);
      hashes_.push_back(
          driver_.RunTick(*states_, frame, log, recorder_, audio_, 0).hash);
    }
  }

  [[nodiscard]] const ComponentRegistry& Registry() const noexcept {
    return module_.Registry();
  }
  [[nodiscard]] GameStateStack& States() noexcept { return *states_; }
  [[nodiscard]] Tick NextTick() const noexcept { return driver_.NextTick(); }
  [[nodiscard]] World& Top() const { return *states_->TopWorlds().front(); }
  [[nodiscard]] const std::vector<std::uint64_t>& Hashes() const noexcept {
    return hashes_;
  }

 private:
  ModuleInstance module_;
  jobs::ForkJoinPool pool_;
  SimDriver driver_;
  ReplayRecorder recorder_;
  NullAudioSink audio_;
  std::unique_ptr<GameStateStack> states_;
  std::vector<std::uint64_t> hashes_;
};

TEST_CASE("While paused, entries apply at once against the next tick",
          "[session_log]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner;
  g_play_running = true;
  PlaySession live;
  live.Start();
  SessionLog log;
  live.RunTo(10, log);
  // Queued for tick 11 while running, then the session pauses before it.
  log.Enqueue(MoverEdit(live.Registry(), 11, 30.0F), live.NextTick(),
              live.States());
  REQUIRE(log.PendingCount() == 1);
  g_play_running = false;
  REQUIRE_FALSE(live.States().SimRunning());

  // Paused edits apply at once, whatever tick their builder named: a stale
  // tick (3) and a future one (999) are both logged against tick 11.
  log.Enqueue(MoverEdit(live.Registry(), 3, 40.0F), live.NextTick(),
              live.States());
  REQUIRE(log.PendingCount() == 0);  // the entry for tick 11 went first
  REQUIRE(log.Applied().size() == 2);
  CHECK(MoverRadius(live.Top()) == 40.0F);
  log.Enqueue(MoverEdit(live.Registry(), 999, 50.0F), live.NextTick(),
              live.States());
  CHECK(MoverRadius(live.Top()) == 50.0F);
  REQUIRE(log.Applied().size() == 3);
  for (const SessionEntry& entry : log.Applied()) {
    REQUIRE(entry.apply_before == 11);
  }
  REQUIRE(log.Failures() == 0);

  g_play_running = true;
  live.RunTo(30, log);
  REQUIRE(live.Hashes().size() == 30);

  // A replay of the written log, never paused, matches tick for tick.
  auto read = SessionLog::Read(log.Write());
  REQUIRE(read.has_value());
  PlaySession replay;
  replay.Start();
  replay.RunTo(30, *read);
  REQUIRE(read->Failures() == 0);
  REQUIRE(read->Applied().size() == 3);
  CHECK(replay.Hashes() == live.Hashes());
  CHECK(MoverRadius(replay.Top()) == 50.0F);

  live.States().Clear();
  replay.States().Clear();
}

TEST_CASE("Every op applies to the top state's Worlds", "[session_log]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner;
  Fixture f;
  f.Start();
  const ComponentRegistry& registry = f.module.Registry();
  SessionLog log;
  const Guid fresh{9, 9};
  log.AddScheduled(
      MakeCreateEntity(
          registry, 1, fresh,
          std::vector<ComponentValue>{{"Transform", R"({"pos":{"x":5,"y":6}})"},
                                      {"Name", R"({"value":"made"})"}})
          .value());
  log.AddScheduled(MakeComponentEntry(registry, SessionOp::kAddComponent, 1,
                                      fresh, "Lifetime",
                                      R"({"expire_tick":99})")
                       .value());
  log.AddScheduled(MakeRemoveComponent(registry, 1, fresh, "Name").value());
  const std::vector<std::pair<Guid, std::vector<ComponentValue>>> batch = {
      {Guid{8, 1}, {{"Transform", R"({})"}}},
      {Guid{8, 2}, {{"Transform", R"({"rot":1})"}}}};
  log.AddScheduled(MakeSpawnBatch(registry, 1, batch).value());
  log.AddScheduled(MakeDestroyEntity(1, MoverGuid(4)));
  log.AddScheduled(MakeAssetSwap(1, Guid{7, 7}, 1234));
  log.ApplyDue(1, *f.states);
  REQUIRE(log.Failures() == 0);
  World& world = f.Top();
  const auto made = world.Find(fresh);
  REQUIRE(made.has_value());
  CHECK(world.CReg().get<Transform>(*made).pos == Vec2{5.0F, 6.0F});
  CHECK(world.CReg().get<testing::Lifetime>(*made).expire_tick == 99);
  CHECK_FALSE(world.CReg().all_of<Name>(*made));
  // Creates are finalized like a load.
  CHECK(world.CReg().all_of<PrevTransform>(*made));
  CHECK(world.Find(Guid{8, 1}).has_value());
  CHECK(world.Find(Guid{8, 2}).has_value());
  CHECK_FALSE(world.Find(MoverGuid(4)).has_value());

  // Entries that cannot apply are counted, not fatal.
  log.AddScheduled(MakeDestroyEntity(2, Guid{123, 456}));
  log.AddScheduled(MakeComponentEntry(registry, SessionOp::kAddComponent, 2,
                                      fresh, "Lifetime", R"({})")
                       .value());
  SessionEntry tampered = MoverEdit(registry, 2, 5.0F);
  tampered.payload.back() = std::byte{0x7F};
  log.AddScheduled(std::move(tampered));
  log.ApplyDue(2, *f.states);
  REQUIRE(log.Failures() == 3);

  // A GameState command applies at its boundary.
  log.AddScheduled(
      MakeGameStateCommand(3, StackOp::kReplace, testing::kGameOverState));
  log.ApplyDue(3, *f.states);
  CHECK(f.states->TopId() == testing::kGameOverState);
  f.states->Clear();
}

TEST_CASE("Play Worlds take session edits through their gate",
          "[session_log]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner;
  Fixture f(WorldKind::kPlay);
  f.Start();
  CHECK(f.Top().Desc().kind == WorldKind::kPlay);
  SessionLog log;
  log.AddScheduled(MoverEdit(f.module.Registry(), 1, 42.0F));
  log.ApplyDue(1, *f.states);
  CHECK(MoverRadius(f.Top()) == 42.0F);
  CHECK_FALSE(f.Top().GateOpen());
  f.states->Clear();
}

TEST_CASE("Write and Read round-trip every entry", "[session_log]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner;
  Fixture f;
  f.Start();
  const ComponentRegistry& registry = f.module.Registry();
  SessionLog log;
  log.AddScheduled(MoverEdit(registry, 5, 1.0F));
  log.AddScheduled(MakeGameStateCommand(9, StackOp::kPush, "x"));
  log.AddScheduled(MakeMarker(9, SessionOp::kStep));
  log.ApplyDue(5, *f.states);  // one applied, two pending
  const std::vector<std::byte> bytes = log.Write();
  auto back = SessionLog::Read(bytes);
  REQUIRE(back.has_value());
  CHECK(back->Applied().empty());
  REQUIRE(back->PendingCount() == 3);
  CHECK(back->Write() == bytes);

  CHECK_FALSE(SessionLog::Read({}).has_value());
  std::vector<std::byte> truncated = bytes;
  truncated.resize(bytes.size() - 3);
  CHECK_FALSE(SessionLog::Read(truncated).has_value());
  f.states->Clear();
}

TEST_CASE("The fixture edits load", "[session_log]") {
  ModuleInstance module(KernelTestModule());
  const auto edits =
      testing::LoadEdits(module.Registry(), testing::EditsFixturePath());
  REQUIRE(edits.has_value());
  REQUIRE(edits->size() == 4);
  REQUIRE((*edits)[0].apply_before == 100);
  REQUIRE((*edits)[3].apply_before == 250);
  CHECK_FALSE(
      testing::LoadEdits(module.Registry(), "no/such/file").has_value());
}

}  // namespace
}  // namespace se
