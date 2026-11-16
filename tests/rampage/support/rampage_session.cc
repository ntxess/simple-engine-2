// Copyright 2026 The simple-engine-2 Authors.

#include "rampage/support/rampage_session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game_rampage/module.h"
#include "game_rampage/scene.h"
#include "se_core/guid.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/rng.h"
#include "se_core/thread_role.h"
#include "se_core/time.h"
#include "se_core/world.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"
#include "se_sim/synthetic_clock.h"

#ifndef SE_SOURCE_DIR
#error "SE_SOURCE_DIR must name the repository root"
#endif

namespace se::testing {
namespace {

constexpr std::uint32_t kInputStream = 0x52414D50;  // "RAMP"
constexpr Tick kSegmentTicks = 40;
// Held masks: none, the four directions and the four diagonals.
constexpr std::array<std::uint64_t, 9> kCombos = {0x0, 0x1, 0x2, 0x4, 0x8,
                                                  0x3, 0x9, 0x6, 0xC};
constexpr std::uint64_t kJump = 0x10;

std::uint64_t HeldAt(Tick tick, std::uint64_t seed) {
  if (tick == 0) return 0;
  const Tick segment = tick / kSegmentTicks;
  const std::uint64_t draw = rng::At(seed, segment, Guid{}, kInputStream, 0);
  std::uint64_t held = kCombos[draw % kCombos.size()];
  if (tick % 97 < 3) held |= kJump;
  return held;
}

const World* TopWorld(const GameStateStack& states) {
  const std::vector<World*> worlds = states.TopWorlds();
  return worlds.empty() ? nullptr : worlds.front();
}

}  // namespace

std::string RampageRepoRoot() { return SE_SOURCE_DIR; }

std::string RampageAssetRoot() { return RampageRepoRoot() + "/assets"; }

std::string RampageFixtureDir() {
  return RampageRepoRoot() + "/tests/fixtures";
}

rampage::SceneRequest SandboxSceneRequest() {
  rampage::SceneRequest request;
  request.format = rampage::SceneRequest::Format::kScene;
  request.path = RampageAssetRoot() + "/scenes/sandbox.scene.json";
  request.asset_root = RampageAssetRoot();
  request.legacy_root = RampageRepoRoot();
  return request;
}

rampage::SceneRequest ReferenceScenarioRequest(const std::string& name) {
  rampage::SceneRequest request;
  request.format = rampage::SceneRequest::Format::kLegacyScenario;
  request.path = RampageFixtureDir() + "/reference/" + name + ".scenario.json";
  request.asset_root = RampageAssetRoot();
  request.legacy_root = RampageRepoRoot();
  return request;
}

InputFrame RampageScriptedInput::Latch(Tick tick, std::uint64_t /*window_end*/,
                                       bool /*last_tick_of_frame*/) {
  return FrameFor(tick, seed_);
}

InputFrame RampageScriptedInput::FrameFor(Tick tick, std::uint64_t seed) {
  InputFrame frame;
  frame.tick = tick;
  frame.held = HeldAt(tick, seed);
  const std::uint64_t before = HeldAt(tick == 0 ? 0 : tick - 1, seed);
  const std::uint64_t pressed = frame.held & ~before;
  const std::uint64_t released = before & ~frame.held;
  for (int a = 0; a < 8; ++a) {
    frame.pressed[a] = static_cast<std::uint8_t>((pressed >> a) & 1U);
    frame.released[a] = static_cast<std::uint8_t>((released >> a) & 1U);
  }
  return frame;
}

RampageSessionResult RunRampageSession(const RampageSessionOptions& options) {
  RampageSessionResult result;
  ModuleInstance module(rampage::RampageModule());
  StateEnv env;
  env.registry = &module.Registry();
  env.seed = options.seed;

  // Load once outside the stack so errors are reported, not just logged.
  {
    const InlineOwnerScope owner(OnMainThread());
    World probe(module.Registry(), {.name = "probe", .seed = options.seed});
    if (auto loaded = rampage::LoadRequestedScene(probe, options.scene);
        !loaded) {
      result.error = loaded.error().message;
      return result;
    }
  }
  rampage::SetSandboxScene(options.scene);
  GameStateStack states(module.Factory(), env);

  ReplayRecorder recorder;
  if (options.record) {
    auto text = rampage::ReadTextFile(options.scene.path);
    const std::string start = text ? *text : std::string();
    const auto bytes = std::as_bytes(std::span<const char>(start));
    const std::vector<std::byte> scene(bytes.begin(), bytes.end());
    recorder.Begin(MakeReplayHeader(scene, options.seed, kDefaultTickRate),
                   scene);
  }

  jobs::ForkJoinPool pool(options.pool);
  SimDriver driver(module.MutableSchedule(), pool);
  NullAudioSink null_audio;
  AudioSink& audio = options.audio != nullptr ? *options.audio : null_audio;
  SessionLog log;
  RampageScriptedInput scripted(options.seed);
  InputSource& input = options.input != nullptr ? *options.input : scripted;
  {
    const InlineOwnerScope owner(OnMainThread());
    states.Request(StackOp::kPush, module.Module().start_state);
    states.ApplyPending();
  }
  if (options.on_world) {
    if (const World* world = TopWorld(states)) options.on_world(*world);
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
    if (!options.on_world) {
      driver.TickInline(states, plan, input, log, recorder, audio);
      for (const TickResult& tick : driver.FrameResults()) {
        result.hashes.push_back(tick.hash);
      }
      continue;
    }
    // TickInline one tick at a time, to see the World after each.
    for (int i = 0; i < plan.steps; ++i) {
      const std::uint64_t window_end =
          plan.tick_window_end_ns[static_cast<std::size_t>(i)];
      TickResult tick;
      {
        const InlineOwnerScope owner(OnMainThread());
        const InputFrame frame =
            input.Latch(driver.NextTick(), window_end, i + 1 == plan.steps);
        tick = driver.RunTick(states, frame, log, recorder, audio, window_end);
      }
      result.hashes.push_back(tick.hash);
      if (const World* world = TopWorld(states)) options.on_world(*world);
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

}  // namespace se::testing
