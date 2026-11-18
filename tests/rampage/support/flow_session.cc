// Copyright 2026 The simple-engine-2 Authors.

#include "rampage/support/flow_session.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/flow.h"
#include "game_rampage/module.h"
#include "game_rampage/scene.h"
#include "rampage/support/rampage_session.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/time.h"
#include "se_core/world.h"
#include "se_sim/content_source.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"
#include "se_sim/synthetic_clock.h"
#include "support/stamped_input.h"

namespace se::testing {
namespace {

using rampage::Action;

StateEnv EnvFor(const ModuleInstance& module, const ContentSource* content,
                std::uint64_t seed) {
  StateEnv env;
  env.registry = &module.Registry();
  env.seed = seed;
  env.content = content;
  return env;
}

void ConfigureFlow(std::string_view gameplay_scene) {
  rampage::FlowConfig config;
  config.gameplay_scene = std::string(gameplay_scene);
  config.content_root = RampageRepoRoot();
  rampage::SetFlowConfig(std::move(config));
}

constexpr ActionId Id(Action action) { return static_cast<ActionId>(action); }

// The flow's recorded session at 120 Hz (the session starts in "title"):
// Enter at the Title, Escape twice (Pause, then Gameplay again), the death
// on the doomed scene (its hunter reaches the player after 128 Gameplay
// ticks), Enter at the GameOver (a retry), Escape (Pause) and Jump (the
// Title), then a few idle Title ticks. Each key is held 4 ticks.
std::vector<StampedInput::ScriptedEdge> FlowScript() {
  std::vector<StampedInput::ScriptedEdge> edges;
  const auto tap = [&edges](Tick tick, Action action) {
    edges.push_back({tick, Id(action), true});
    edges.push_back({tick + 4, Id(action), false});
  };
  tap(20, Action::kEnter);
  // Walk right for a while: the Gameplay World's hashes depend on input.
  edges.push_back({30, Id(Action::kRight), true});
  tap(50, Action::kExit);
  edges.push_back({60, Id(Action::kRight), false});
  tap(80, Action::kExit);
  tap(240, Action::kEnter);
  tap(280, Action::kExit);
  tap(310, Action::kJump);
  return edges;
}

}  // namespace

std::string FlowInputPath() {
  return RampageFixtureDir() + "/flow/flow.seinput";
}

std::unique_ptr<CachedContentSource> RepoContent() {
  std::string root = RampageRepoRoot();
  return std::make_unique<CachedContentSource>([root](std::string_view path) {
    return rampage::ReadTextFile(root + "/" + std::string(path));
  });
}

InputFrame PressFrame(Tick tick, Action action) {
  InputFrame frame;
  frame.tick = tick;
  frame.held = 1ULL << Id(action);
  frame.pressed[Id(action)] = 1;
  return frame;
}

FlowHarness::FlowHarness(std::string_view gameplay_scene)
    : module_(rampage::RampageModule()),
      content_(RepoContent()),
      states_(module_.Factory(), EnvFor(module_, content_.get(), 42)),
      pool_(jobs::ForkJoinPool::Config{}),
      driver_(module_.MutableSchedule(), pool_) {
  ConfigureFlow(gameplay_scene);
}

// The harness lives inside a test's MainThreadScope.
FlowHarness::~FlowHarness() SE_NO_THREAD_SAFETY_ANALYSIS {
  const InlineOwnerScope owner(OnMainThread());
  states_.Clear();
}

void FlowHarness::Start(std::string_view state) {
  const InlineOwnerScope owner(OnMainThread());
  states_.Request(StackOp::kPush, state);
  states_.ApplyPending();
}

TickResult FlowHarness::Tick(InputFrame frame) {
  frame.tick = driver_.NextTick();
  const InlineOwnerScope owner(OnMainThread());
  return driver_.RunTick(states_, frame, log_, recorder_, audio_, 0);
}

TickResult FlowHarness::Idle() { return Tick(InputFrame{}); }

TickResult FlowHarness::Press(Action action) {
  return Tick(PressFrame(0, action));
}

void FlowHarness::ApplyPending() {
  const InlineOwnerScope owner(OnMainThread());
  states_.ApplyPending();
}

std::vector<std::string> FlowHarness::Ids() const {
  std::vector<std::string> ids;
  for (const StackEntry& entry : states_.Entries()) {
    ids.emplace_back(entry.id);
  }
  return ids;
}

FlowSessionResult RunFlowSession(const FlowSessionOptions& options) {
  FlowSessionResult result;
  ModuleInstance module(rampage::RampageModule());
  const std::unique_ptr<CachedContentSource> content = RepoContent();
  ConfigureFlow(options.gameplay_scene);
  if (auto scene = content->Text(options.gameplay_scene); !scene) {
    result.error = scene.error().message;
    return result;
  }
  GameStateStack states(module.Factory(),
                        EnvFor(module, content.get(), options.seed));

  ReplayRecorder recorder;
  if (options.record) {
    const std::string_view start = *content->Text(options.gameplay_scene);
    const auto bytes = std::as_bytes(std::span<const char>(start));
    const std::vector<std::byte> scene(bytes.begin(), bytes.end());
    recorder.Begin(MakeReplayHeader(scene, options.seed, kDefaultTickRate),
                   scene);
  }
  jobs::ForkJoinPool pool(jobs::ForkJoinPool::Config{});
  SimDriver driver(module.MutableSchedule(), pool);
  NullAudioSink audio;
  SessionLog log;
  RampageScriptedInput idle(options.seed);
  InputSource& input = options.input != nullptr ? *options.input : idle;
  {
    const InlineOwnerScope owner(OnMainThread());
    states.Request(StackOp::kPush, options.start_state);
    states.ApplyPending();
  }

  FixedStepper stepper;
  SyntheticClockConfig clock_config;
  clock_config.hz = options.hz > 0.0 ? options.hz : 120.0;
  clock_config.seed = options.clock_seed;
  clock_config.jitter_sec = options.jitter_sec;
  SyntheticFrameClock clock(clock_config);
  const FrameCosts costs;
  while (driver.NextTick() <= options.ticks) {
    StepPlan plan;
    if (options.hz > 0.0) {
      plan = stepper.Plan(clock.Next(), costs, states.SimRunning(),
                          /*debugger=*/false, /*editor_play=*/false);
      stepper.OnPresentResult(false, false);
    } else {
      plan.steps = 1;
    }
    const Tick remaining = options.ticks - driver.NextTick() + 1;
    plan.steps = static_cast<int>(
        std::min<Tick>(static_cast<Tick>(plan.steps), remaining));
    result.steps_per_frame.push_back(plan.steps);
    // An empty stack does not run (SimRunning is false): the session ends.
    if (options.hz > 0.0 && plan.steps == 0 && states.Depth() == 0) break;
    driver.TickInline(states, plan, input, log, recorder, audio);
    for (const TickResult& tick : driver.FrameResults()) {
      result.hashes.push_back(tick.hash);
      result.tops.push_back(tick.top_state);
      result.depths.push_back(states.Depth());
    }
  }
  if (options.record) {
    recorder.RecordSession(log);
    result.replay = recorder.Finish();
  }
  {
    const InlineOwnerScope owner(OnMainThread());
    states.Clear();
  }
  return result;
}

std::vector<InputFrame> FlowScriptFrames(Tick ticks) {
  // The InputService's windows at 120 Hz, one tick per frame: tick t's
  // window ends at epoch + t * dt.
  constexpr std::uint64_t kEpochNs = 1'000'000'000;
  StampedInput input(FlowScript(), kEpochNs);
  std::vector<InputFrame> frames;
  frames.reserve(ticks);
  for (Tick tick = 1; tick <= ticks; ++tick) {
    const std::uint64_t end =
        kEpochNs + tick * 1'000'000'000ULL / kDefaultTickRate;
    frames.push_back(input.Latch(tick, end, /*last_tick_of_frame=*/true));
  }
  return frames;
}

std::string WriteSeinput(const std::vector<InputFrame>& frames) {
  std::string text =
      "# seinput v1 rate=120\n"
      "# actions (bit 0 first): Up Left Down Right Jump Exit Enter Back A1 A2 "
      "A3 A4 A5 A6\n"
      "# tick held pressed released (hex); generated by "
      "tests/rampage/flow_input_gen.cc\n";
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const InputFrame& frame = frames[i];
    std::uint64_t pressed = 0;
    std::uint64_t released = 0;
    for (std::size_t a = 0; a < kMaxActions; ++a) {
      if (frame.pressed[a] > 0) pressed |= 1ULL << a;
      if (frame.released[a] > 0) released |= 1ULL << a;
    }
    const bool last = i + 1 == frames.size();
    if (pressed == 0 && released == 0 && !last) continue;
    text += std::format("{:x} {:x} {:x} {:x}\n", frame.tick, frame.held,
                        pressed, released);
  }
  return text;
}

}  // namespace se::testing
