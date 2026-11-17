// Copyright 2026 The simple-engine-2 Authors.
//
// The stack's content gate and content bundles (docs/milestone-specs.md
// M13a): a push that waits at the gate applies before the same tick as
// without one, the commands behind it keep their order, and a content
// bundle with the session's kAssetSwap entries replays a multi-state
// session whose texts changed. The content test module
// (support/content_test_module.h) builds its Worlds from texts.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/time.h"
#include "se_core/world.h"
#include "se_sim/content_bundle.h"
#include "se_sim/content_source.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"
#include "support/content_test_module.h"
#include "support/module_session.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::ContentFixtureSource;
using testing::ContentTestModule;
using testing::kContentPause;
using testing::kContentPauseKey;
using testing::kContentPlay;
using testing::kContentQuit;
using testing::kContentStart;
using testing::kContentTitle;
using testing::MainThreadScope;
using testing::ScriptedPresses;

// Title, then play (tick 5), pause (20), resume (30), pause (45), back to
// the title (60): every kind of transition of the module.
std::vector<ScriptedPresses::Press> FlowPresses() {
  return {{5, kContentStart},
          {20, kContentPauseKey},
          {30, kContentPauseKey},
          {45, kContentPauseKey},
          {60, kContentQuit}};
}

// Holds every state for |frames| frames from the first time it is asked;
// counts the frames itself (the runtime's asset loads, without a lane).
class FramesGate final : public StateContentGate {
 public:
  explicit FramesGate(int frames) : frames_(frames) {}

  bool Ready(std::string_view state_id) override {
    ++asks_;
    auto [it, inserted] = asked_.try_emplace(std::string(state_id), frame_);
    if (frame_ - it->second >= frames_) {
      asked_.erase(it);
      return true;
    }
    return false;
  }
  void NextFrame() { ++frame_; }
  [[nodiscard]] int Asks() const noexcept { return asks_; }

 private:
  int frames_;
  int frame_ = 0;
  int asks_ = 0;
  std::map<std::string, int, std::less<>> asked_;
};

// The runtime's TickGate: no tick while a state waits.
class WaitGate final : public TickGate {
 public:
  bool HoldTicks(const GameStateStack& states, Tick /*next_tick*/) override {
    return !states.Waiting().empty();
  }
};

struct Run {
  std::vector<std::uint64_t> hashes;
  std::vector<std::string> tops;
  int held_frames = 0;
};

// The App's frame loop, reduced: two ticks per frame through TickInline
// with |gate| on the stack (null: none), a frame that waits completing the
// boundary again first.
Run RunFlow(FramesGate* gate) SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner) {
  ModuleInstance module(ContentTestModule());
  const CachedContentSource content = ContentFixtureSource();
  StateEnv env;
  env.registry = &module.Registry();
  env.seed = 7;
  env.content = &content;
  GameStateStack states(module.Factory(), env);
  if (gate != nullptr) states.SetContentGate(gate);
  jobs::ForkJoinPool pool(jobs::ForkJoinPool::Config{});
  SimDriver driver(module.MutableSchedule(), pool);
  WaitGate wait;
  driver.SetTickGate(&wait);
  SessionLog log;
  ReplayRecorder recorder;
  NullAudioSink audio;
  ScriptedPresses input(FlowPresses());
  Run run;
  {
    const InlineOwnerScope owner(OnMainThread());
    states.Request(StackOp::kPush, kContentTitle);
    states.ApplyPending();
  }
  while (driver.NextTick() <= 80) {
    if (gate != nullptr) gate->NextFrame();
    if (!states.Waiting().empty()) {
      const InlineOwnerScope owner(OnMainThread());
      driver.CompleteBoundary(states, log);
    }
    if (!states.Waiting().empty()) {
      ++run.held_frames;
      continue;
    }
    StepPlan plan;
    plan.steps = 2;
    driver.TickInline(states, plan, input, log, recorder, audio);
    if (driver.HeldLastFrame()) ++run.held_frames;
    for (const TickResult& tick : driver.FrameResults()) {
      run.hashes.push_back(tick.hash);
      run.tops.push_back(tick.top_state);
    }
  }
  const InlineOwnerScope owner(OnMainThread());
  states.Clear();
  return run;
}

TEST_CASE("A push waits at the content gate and applies before the same tick",
          "[content]") {
  const MainThreadScope main;
  const Run plain = RunFlow(nullptr);
  REQUIRE(plain.hashes.size() >= 80);
  REQUIRE(plain.held_frames == 0);
  // The flow went through every state.
  CHECK(plain.tops[3] == kContentTitle);
  CHECK(plain.tops[10] == kContentPlay);
  CHECK(plain.tops[25] == kContentPause);
  CHECK(plain.tops[35] == kContentPlay);
  CHECK(plain.tops[70] == kContentTitle);
  for (const int frames : {0, 1, 7}) {
    FramesGate gate(frames);
    const Run gated = RunFlow(&gate);
    INFO("the gate holds each state for " << frames << " frames");
    REQUIRE(gate.Asks() > 0);
    if (frames > 0) REQUIRE(gated.held_frames > 0);
    REQUIRE(gated.hashes.size() >= 80);
    for (std::size_t i = 0; i < 80; ++i) {
      INFO("tick " << i + 1);
      REQUIRE(gated.hashes[i] == plain.hashes[i]);
      REQUIRE(gated.tops[i] == plain.tops[i]);
    }
  }
}

TEST_CASE("Commands behind a waiting push keep their order", "[content]") {
  const MainThreadScope main;
  ModuleInstance module(ContentTestModule());
  const CachedContentSource content = ContentFixtureSource();
  StateEnv env;
  env.registry = &module.Registry();
  env.content = &content;
  GameStateStack states(module.Factory(), env);
  FramesGate gate(1);
  states.SetContentGate(&gate);
  const InlineOwnerScope owner(OnMainThread());
  states.Request(StackOp::kPush, kContentPlay);
  states.ApplyPending();
  REQUIRE(states.Waiting() == kContentPlay);
  gate.NextFrame();
  states.ApplyPending();
  REQUIRE(states.Waiting().empty());
  REQUIRE(states.TopId() == kContentPlay);

  // A World's request (collected at the boundary) waits at the gate; the
  // stack's pop and replace queued behind it wait with it.
  RequestGameState(*states.TopWorlds().front(), StackOp::kPush, kContentPause);
  states.ApplyPending();
  CHECK(states.Waiting() == kContentPause);
  states.Request(StackOp::kPop);
  states.Request(StackOp::kReplace, kContentTitle);
  CHECK(states.Waiting() == kContentPause);
  REQUIRE(states.PendingCount() == 3);
  CHECK(states.TopId() == kContentPlay);
  REQUIRE(states.Depth() == 1);
  // Asking again in the same frame changes nothing.
  states.ApplyPending();
  CHECK(states.Waiting() == kContentPause);
  REQUIRE(states.PendingCount() == 3);
  // Next frame: pause is pushed, then popped; the title replace waits in
  // turn.
  gate.NextFrame();
  const std::uint32_t before = states.Revision();
  states.ApplyPending();
  CHECK(states.Revision() == before + 2);
  CHECK(states.TopId() == kContentPlay);
  CHECK(states.Waiting() == kContentTitle);
  REQUIRE(states.PendingCount() == 1);
  gate.NextFrame();
  states.ApplyPending();
  CHECK(states.Waiting().empty());
  CHECK(states.TopId() == kContentTitle);
  REQUIRE(states.Depth() == 1);
  REQUIRE(states.RejectedCount() == 0);
  // A pop never waits, and an unknown state is rejected, not asked about.
  const int asks = gate.Asks();
  states.Request(StackOp::kPush, "no_such_state");
  states.Request(StackOp::kPop);
  states.ApplyPending();
  CHECK(gate.Asks() == asks);
  REQUIRE(states.RejectedCount() == 1);
  REQUIRE(states.Depth() == 0);
  states.Clear();
}

// A SessionLog entry due at a boundary where a push waits applies after the
// push, to the new top, as in a replay without a gate.
TEST_CASE("Session entries wait behind a push at the content gate",
          "[content]") {
  const MainThreadScope main;
  ModuleInstance module(ContentTestModule());
  const CachedContentSource content = ContentFixtureSource();
  StateEnv env;
  env.registry = &module.Registry();
  env.content = &content;
  GameStateStack states(module.Factory(), env);
  FramesGate gate(2);
  states.SetContentGate(&gate);
  SessionLog log;
  const InlineOwnerScope owner(OnMainThread());
  states.Request(StackOp::kPush, kContentPlay);
  log.AddScheduled(MakeGameStateCommand(1, StackOp::kPush, kContentPause));
  log.AddScheduled(MakeGameStateCommand(1, StackOp::kPop, {}));
  states.ApplyPending();
  log.ApplyDue(1, states);
  CHECK(log.Applied().empty());
  REQUIRE(states.Depth() == 0);
  gate.NextFrame();
  gate.NextFrame();
  states.ApplyPending();
  REQUIRE(states.TopId() == kContentPlay);
  log.ApplyDue(1, states);
  // The pause push waits in turn; the pop behind it stays in the log.
  REQUIRE(log.Applied().size() == 1);
  REQUIRE(log.PendingCount() == 1);
  CHECK(states.Waiting() == kContentPause);
  gate.NextFrame();
  gate.NextFrame();
  states.ApplyPending();
  log.ApplyDue(1, states);
  REQUIRE(log.PendingCount() == 0);
  CHECK(states.TopId() == kContentPlay);
  REQUIRE(states.Depth() == 1);
  REQUIRE(log.Failures() == 0);
  states.Clear();
}

// A content source over texts held in memory, which a test may change; it
// records every version it serves and reports a changed one, as the
// runtime's AssetRuntime does (a kAssetSwap entry before the tick).
class ChangingSource final : public ContentSource {
 public:
  using OnSwap = std::function<void(std::string_view, std::uint64_t)>;

  std::expected<std::string_view, Error> Text(
      std::string_view path) const override {
    const auto found = texts_.find(path);
    if (found == texts_.end()) {
      return std::unexpected(
          Error{ErrorCode::kNotFound, "no text", std::string(path), 0, 0});
    }
    const ContentText* before = bundle_.Latest(path);
    const std::uint64_t hash = ContentTextHash(found->second);
    if (before != nullptr && before->hash != hash && on_swap_) {
      on_swap_(path, hash);
    }
    bundle_.Add(path, found->second);
    return std::string_view(found->second);
  }

  void Set(std::string_view path, std::string text) {
    texts_[std::string(path)] = std::move(text);
  }
  void SetOnSwap(OnSwap on_swap) { on_swap_ = std::move(on_swap); }
  [[nodiscard]] const ContentBundle& Served() const { return bundle_; }

 private:
  std::map<std::string, std::string, std::less<>> texts_;
  mutable ContentBundle bundle_;
  OnSwap on_swap_;
};

TEST_CASE("A content bundle replays a multi-state session", "[content]") {
  const MainThreadScope main;
  const CachedContentSource fixtures = ContentFixtureSource();
  ChangingSource source;
  for (const std::string_view path :
       {"content/title.txt", "content/play.txt", "content/common.txt",
        "content/pause.txt"}) {
    auto text = fixtures.Text(path);
    REQUIRE(text.has_value());
    source.Set(path, std::string(*text));
  }

  // The live session: one tick per frame, the texts served by |source|;
  // the pause overlay's text changes at tick 35, so the second pause (45)
  // reads the new version, logged as a kAssetSwap entry.
  ModuleInstance module(ContentTestModule());
  StateEnv env;
  env.registry = &module.Registry();
  env.seed = 9;
  env.content = &source;
  GameStateStack states(module.Factory(), env);
  jobs::ForkJoinPool pool(jobs::ForkJoinPool::Config{});
  SimDriver driver(module.MutableSchedule(), pool);
  SessionLog log;
  source.SetOnSwap([&](std::string_view path, std::uint64_t hash) {
    log.AddScheduled(
        MakeAssetSwap(driver.NextTick(), ContentPathGuid(path), hash));
  });
  ReplayRecorder recorder;
  NullAudioSink audio;
  ScriptedPresses presses(FlowPresses());
  recorder.Begin(MakeReplayHeader({}, env.seed, kDefaultTickRate), {});
  std::vector<std::uint64_t> live;
  {
    const InlineOwnerScope owner(OnMainThread());
    states.Request(StackOp::kPush, kContentTitle);
    while (driver.NextTick() <= 80) {
      if (driver.NextTick() == 35) {
        source.Set("content/pause.txt",
                   "entity veil 0 0 0 0\nentity glow 5 5 1 1\n");
      }
      const InputFrame frame = presses.Latch(driver.NextTick(), 0, true);
      live.push_back(
          driver.RunTick(states, frame, log, recorder, audio, 0).hash);
    }
    states.Clear();
  }
  REQUIRE(source.Served().Texts().size() == 5);
  ContentBundle bundle = source.Served();
  bundle.SetStartState(kContentTitle);
  recorder.SetStart(bundle.Write());
  recorder.RecordSession(log);
  const std::vector<std::byte> file = recorder.Finish();

  // The replay: the bundle from the file's start bytes, its frames and log.
  auto reader = ReplayReader::Open(file);
  REQUIRE(reader.has_value());
  REQUIRE(ContentBundle::IsBundle(reader->StartBytes()));
  auto read = ContentBundle::Read(reader->StartBytes());
  REQUIRE(read.has_value());
  CHECK(*read == bundle);
  CHECK(read->StartState() == kContentTitle);
  ContentBundleSource replay_source(std::move(*read));
  testing::ModuleSessionOptions options;
  options.module = &ContentTestModule();
  options.start_state = std::string(replay_source.Bundle().StartState());
  options.bundle = &replay_source;
  options.seed = reader->Header().seed;
  options.ticks = 80;
  options.input = &*reader;
  const std::span<const std::byte> session = reader->SessionBytes();
  options.session.assign(session.begin(), session.end());
  const testing::ModuleSessionResult replayed =
      testing::RunModuleSession(options);
  REQUIRE(replayed.error.empty());
  REQUIRE(replayed.swaps_selected == 1);
  REQUIRE(replayed.session_failures == 0);
  REQUIRE(replayed.hashes == live);
  for (std::size_t i = 0; i < live.size(); ++i) {
    REQUIRE(reader->Hashes()[i] == live[i]);
  }

  // Without the swap the second pause reads the first version: the
  // hashes diverge from tick 45 on.
  ContentBundleSource unswapped(bundle);
  options.bundle = &unswapped;
  options.session.clear();
  const testing::ModuleSessionResult stale = testing::RunModuleSession(options);
  REQUIRE(stale.hashes.size() == live.size());
  CHECK(stale.hashes[40] == live[40]);
  CHECK(stale.hashes[50] != live[50]);
}

TEST_CASE("A content bundle round-trips its texts and rejects a bad one",
          "[content]") {
  ContentBundle bundle;
  CHECK(bundle.Add("a/b.json", "{\"x\": \"q\\\"\\n\\t\"}\r\n"));
  CHECK_FALSE(bundle.Add("a/b.json", "{\"x\": \"q\\\"\\n\\t\"}\r\n"));
  CHECK(bundle.Add("a/b.json", std::string("caf\xC3\xA9 \x01 end", 11)));
  bundle.SetStartState("title");
  const std::vector<std::byte> bytes = bundle.Write();
  REQUIRE(ContentBundle::IsBundle(bytes));
  auto read = ContentBundle::Read(bytes);
  REQUIRE(read.has_value());
  CHECK(*read == bundle);
  CHECK(read->Latest("a/b.json")->text ==
        std::string("caf\xC3\xA9 \x01 end", 11));

  const std::string_view scene = R"({"format": "se-scene-v1"})";
  CHECK_FALSE(ContentBundle::IsBundle(std::as_bytes(std::span(scene))));
  const std::string_view bad =
      "{\"format\": \"se-content-bundle-v1\",\n \"texts\": [{\"path\": 1}]}";
  auto failed = ContentBundle::Read(std::as_bytes(std::span(bad)));
  REQUIRE_FALSE(failed.has_value());
  REQUIRE(failed.error().line == 2);
  std::string tampered(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
  tampered.replace(tampered.find("caf"), 3, "CAF");
  CHECK_FALSE(
      ContentBundle::Read(std::as_bytes(std::span(tampered))).has_value());
}

}  // namespace
}  // namespace se
