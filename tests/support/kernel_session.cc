// Copyright 2026 The simple-engine-2 Authors.

#include "support/kernel_session.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "se_core/guid.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/time.h"
#include "se_core/world.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"
#include "se_sim/synthetic_clock.h"
#include "support/kernel_test_module.h"

namespace se::testing {
namespace {

// The first Guid whose digest differs, for the cross-check report.
std::string FirstDifference(const std::vector<EntityDigest>& expected,
                            const std::vector<EntityDigest>& actual) {
  const std::size_t n = std::min(expected.size(), actual.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (expected[i] != actual[i]) {
      return std::format("entity {}: {:016x} vs {:016x}",
                         ToString(expected[i].guid), expected[i].digest,
                         actual[i].digest);
    }
  }
  return std::format("entity counts differ: {} vs {}", expected.size(),
                     actual.size());
}

// One tick through RunTick with the cross-check around it.
TickResult CrossCheckedTick(SimDriver& driver, Schedule& schedule,
                            GameStateStack& states, const InputFrame& frame,
                            SessionLog& log, ReplayRecorder& recorder,
                            AudioSink& audio, std::uint64_t window_end,
                            std::uint32_t every, KernelSessionResult& result)
    SE_REQUIRES(g_sim_owner) {
  const bool check = every > 0 && frame.tick % every == 0;
  std::vector<std::unique_ptr<World>> clones;
  const std::string top_before(states.TopId());
  const std::size_t applied_before = log.Applied().size();
  const std::size_t pending_before = states.PendingCount();
  if (check) {
    for (World* world : states.RunningWorlds()) {
      clones.push_back(world->Clone());
    }
  }
  const TickResult tick =
      driver.RunTick(states, frame, log, recorder, audio, window_end);
  if (!check) return tick;
  if (log.Applied().size() != applied_before || states.TopId() != top_before ||
      pending_before != 0 || clones.empty()) {
    // Boundary work happened that a bare Step of the clone would not see.
    ++result.crosschecks_skipped;
    return tick;
  }
  jobs::ForkJoinPool serial;
  const std::vector<World*> worlds = states.RunningWorlds();
  for (std::size_t i = 0; i < clones.size() && i < worlds.size(); ++i) {
    schedule.Step(*clones[i], frame, serial);
    const std::vector<EntityDigest> expected = clones[i]->Digests();
    const std::vector<EntityDigest> actual = worlds[i]->Digests();
    if (expected != actual && result.crosscheck_ok) {
      result.crosscheck_ok = false;
      result.crosscheck_report = std::format("tick {}: {}", frame.tick,
                                             FirstDifference(expected, actual));
    }
  }
  ++result.crosschecks;
  return tick;
}

}  // namespace

KernelSessionResult RunKernelSession(const KernelSessionOptions& options) {
  KernelSessionResult result;
  ModuleInstance module(KernelTestModule());
  StateEnv env;
  env.registry = &module.Registry();
  env.seed = options.seed;
  GameStateStack states(module.Factory(), env);

  SessionLog log;
  if (options.replay != nullptr) {
    auto read = SessionLog::Read(options.replay->SessionBytes());
    if (!read) {
      result.error = read.error().message;
      return result;
    }
    log = std::move(*read);
  } else if (!options.edits_path.empty()) {
    auto edits = LoadEdits(module.Registry(), options.edits_path);
    if (!edits) {
      result.error = edits.error().message;
      return result;
    }
    for (SessionEntry& entry : *edits) log.AddScheduled(std::move(entry));
  }

  ReplayRecorder recorder;
  if (options.record) {
    const std::vector<std::byte> scene = SceneBytes(options.seed);
    recorder.Begin(MakeReplayHeader(scene, options.seed, kDefaultTickRate),
                   scene);
  }

  jobs::ForkJoinPool pool;
  SimDriver driver(module.MutableSchedule(), pool);
  NullAudioSink audio;
  ScriptedInput scripted(options.seed);
  InputSource& input = options.replay != nullptr
                           ? static_cast<InputSource&>(*options.replay)
                           : static_cast<InputSource&>(scripted);
  {
    const InlineOwnerScope owner;
    states.Request(StackOp::kPush, module.Module().start_state);
    states.ApplyPending();
  }

  FixedStepper stepper;
  SyntheticFrameClock clock(options.hz > 0.0 ? options.hz : 120.0,
                            options.clock_seed, options.jitter_sec);
  const FrameCosts costs;  // no measurements: the cap is k_rt + 1
  while (driver.NextTick() <= options.ticks) {
    if (!states.SimRunning()) {
      result.error = "the simulation stopped running";
      break;
    }
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

    std::vector<TickResult> ticks;
    if (options.det_crosscheck > 0) {
      const InlineOwnerScope owner;
      for (int i = 0; i < plan.steps; ++i) {
        const std::uint64_t window_end =
            plan.tick_window_end_ns[static_cast<std::size_t>(i)];
        const InputFrame frame =
            input.Latch(driver.NextTick(), window_end, i + 1 == plan.steps);
        ticks.push_back(CrossCheckedTick(
            driver, module.MutableSchedule(), states, frame, log, recorder,
            audio, window_end, options.det_crosscheck, result));
      }
    } else {
      driver.TickInline(states, plan, input, log, recorder, audio);
      ticks.assign(driver.FrameResults().begin(), driver.FrameResults().end());
    }
    for (const TickResult& tick : ticks) {
      result.hashes.push_back(tick.hash);
      result.top_states.push_back(tick.top_state);
      if (options.on_tick) options.on_tick(tick.tick, states);
    }
  }

  result.session_applied = log.Applied().size();
  result.session_failures = log.Failures();
  if (options.record) {
    recorder.RecordSession(log);
    result.replay = recorder.Finish();
  }
  {
    const InlineOwnerScope owner;
    states.Clear();
  }
  return result;
}

}  // namespace se::testing
