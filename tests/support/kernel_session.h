// Copyright 2026 The simple-engine-2 Authors.
//
// Runs the kernel test module headless, the way the runtime would: frames
// from a SyntheticFrameClock (or one tick per frame), a FixedStepper plan per
// frame, SimDriver::TickInline, the fixture's logged edits, and optionally a
// replay recording, a replay or another InputSource as input, and
// --det-crosscheck. Shared by the determinism tests and tools/se_replay.

#ifndef TESTS_SUPPORT_KERNEL_SESSION_H_
#define TESTS_SUPPORT_KERNEL_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "se_core/input_frame.h"
#include "se_core/input_frame_ring.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/time.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/sim_driver.h"

namespace se::testing {

struct KernelSessionOptions {
  // Display rate of the synthetic clock; 0 runs one tick per frame.
  double hz = 0.0;
  double jitter_sec = 0.0;
  std::uint64_t clock_seed = 7;
  // Irregular clocks (SyntheticClockConfig): missed vblanks, unknown vblank
  // counts, and a reported timer p99 below the jitter (negative: the
  // jitter), which stops Free-cadence deltas from snapping.
  double miss_probability = 0.0;
  bool vblanks_known = true;
  double timer_p99_sec = -1.0;
  // World seed (scene generation, spawn phases, scripted input).
  std::uint64_t seed = 42;
  // kPlay runs the editor's Play-World gate checks.
  WorldKind world_kind = WorldKind::kRuntime;
  Tick ticks = 600;
  // Logged edits to schedule; empty for none.
  std::string edits_path;
  // Input and session log from a replay instead of ScriptedInput.
  ReplayReader* replay = nullptr;
  // Input from this source instead of ScriptedInput (not with a replay).
  InputSource* input = nullptr;
  bool record = false;
  // Every K ticks, step a clone of each running World serially and compare
  // per-entity digests with the real tick (0: off).
  std::uint32_t det_crosscheck = 0;
  // Attached to the SimDriver (InputFrameRing) when set.
  InputFrameRing* input_ring = nullptr;
  // Called after every tick.
  std::function<void(Tick, const GameStateStack&)> on_tick;
  // Asked before every tick TickInline runs (M9's asset hold).
  TickGate* gate = nullptr;
  // The session's ForkJoinPool (M14; W = 0 by default).
  jobs::ForkJoinPool::Config pool{};
};

struct KernelSessionResult {
  // hashes[i] is the hash after tick i + 1.
  std::vector<std::uint64_t> hashes;
  std::vector<std::string> top_states;
  std::vector<int> steps_per_frame;
  // The FixedStepper's counters (dilation, stalls) at the end.
  StepperStats stepper;
  std::size_t session_applied = 0;
  std::uint32_t session_failures = 0;
  std::vector<std::byte> replay;
  std::uint32_t crosschecks = 0;
  std::uint32_t crosschecks_skipped = 0;
  bool crosscheck_ok = true;
  std::string crosscheck_report;
  // At the first divergence: the serial clone's and the real World's
  // per-entity digests, sorted by Guid.
  std::vector<EntityDigest> crosscheck_expected;
  std::vector<EntityDigest> crosscheck_actual;
  std::string error;
};

[[nodiscard]] KernelSessionResult RunKernelSession(
    const KernelSessionOptions& options) SE_REQUIRES(g_main)
    SE_EXCLUDES(g_sim_owner);

}  // namespace se::testing

#endif  // TESTS_SUPPORT_KERNEL_SESSION_H_
