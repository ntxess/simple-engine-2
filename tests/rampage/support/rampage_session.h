// Copyright 2026 The simple-engine-2 Authors.
//
// Runs the rampage module headless the way the runtime would: the sandbox
// state loaded from a SceneRequest, frames from a SyntheticFrameClock (or
// one tick per frame), a FixedStepper plan per frame and SimDriver ticks,
// with input from an InputSource (scripted by default). Shared by the
// rampage tests and tools/se_replay.

#ifndef TESTS_RAMPAGE_SUPPORT_RAMPAGE_SESSION_H_
#define TESTS_RAMPAGE_SUPPORT_RAMPAGE_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game_rampage/module.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_sim/sim_driver.h"

namespace se::testing {

// The repository's root, assets/ and tests/fixtures/ directories.
[[nodiscard]] std::string RampageRepoRoot();
[[nodiscard]] std::string RampageAssetRoot();
[[nodiscard]] std::string RampageFixtureDir();

// assets/scenes/sandbox.scene.json.
[[nodiscard]] rampage::SceneRequest SandboxSceneRequest();
// tests/fixtures/reference/<name>.scenario.json.
[[nodiscard]] rampage::SceneRequest ReferenceScenarioRequest(
    const std::string& name);

// Input as a pure function of the tick and a seed: 40-tick segments, each
// holding a seeded combination of Up, Left, Down and Right (never two
// opposite directions), and Jump for 3 ticks every 97.
class RampageScriptedInput final : public InputSource {
 public:
  explicit RampageScriptedInput(std::uint64_t seed) noexcept : seed_(seed) {}
  InputFrame Latch(Tick tick, std::uint64_t tick_window_end_ns,
                   bool last_tick_of_frame) override;
  [[nodiscard]] static InputFrame FrameFor(Tick tick, std::uint64_t seed);

 private:
  std::uint64_t seed_;
};

struct RampageSessionOptions {
  rampage::SceneRequest scene;
  // Display rate of the synthetic clock; 0 runs one tick per frame.
  double hz = 0.0;
  double jitter_sec = 0.0;
  std::uint64_t clock_seed = 7;
  // The World seed (StateEnv) and the scripted input's seed.
  std::uint64_t seed = 42;
  Tick ticks = 3600;
  // Input from this source instead of RampageScriptedInput.
  InputSource* input = nullptr;
  // The tick's audio events go here instead of a NullAudioSink (M8).
  AudioSink* audio = nullptr;
  // Record a replay (start bytes: the scene file).
  bool record = false;
  // Called with the sandbox World after the load (tick 0) and after every
  // tick; ticks then run one at a time.
  std::function<void(const World&)> on_world;
  // The session's ForkJoinPool (M14; W = 0 by default).
  jobs::ForkJoinPool::Config pool{};
};

struct RampageSessionResult {
  // hashes[i] is the hash after tick i + 1.
  std::vector<std::uint64_t> hashes;
  std::vector<int> steps_per_frame;
  std::vector<std::byte> replay;
  std::string error;
};

[[nodiscard]] RampageSessionResult RunRampageSession(
    const RampageSessionOptions& options) SE_REQUIRES(g_main)
    SE_EXCLUDES(g_sim_owner);

}  // namespace se::testing

#endif  // TESTS_RAMPAGE_SUPPORT_RAMPAGE_SESSION_H_
