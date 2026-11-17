// Copyright 2026 The simple-engine-2 Authors.
//
// rampage's game flow headless (M13b): a GameStateStack of the module with
// StateEnv::content a CachedContentSource over the repository (the
// runtime's lane-loaded texts read synchronously), driven tick by tick
// (FlowHarness, for the rules' tests) or by a synthetic frame clock and a
// FixedStepper like RunRampageSession (RunFlowSession, for the determinism
// golden). The flow's scenes are content-root paths; the repository is the
// content root.

#ifndef TESTS_RAMPAGE_SUPPORT_FLOW_SESSION_H_
#define TESTS_RAMPAGE_SUPPORT_FLOW_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "game_rampage/flow.h"
#include "game_rampage/module.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_sim/content_source.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"

namespace se::testing {

// tests/fixtures/flow/doomed.scene.json, as a content-root path.
inline constexpr std::string_view kDoomedScene =
    "tests/fixtures/flow/doomed.scene.json";
// tests/fixtures/flow/flow.seinput (absolute).
[[nodiscard]] std::string FlowInputPath();

// The repository's files, read on first use.
[[nodiscard]] std::unique_ptr<CachedContentSource> RepoContent();

// An InputFrame of |tick| with a press (and the held bit) of |action|.
[[nodiscard]] InputFrame PressFrame(Tick tick, rampage::Action action);

// The module's stack, ticked one InputFrame at a time.
class FlowHarness {
 public:
  // The flow reads |gameplay_scene| for "gameplay" (rampage::SetFlowConfig).
  explicit FlowHarness(
      std::string_view gameplay_scene = rampage::kDefaultGameplayScene);
  FlowHarness(const FlowHarness&) = delete;
  FlowHarness& operator=(const FlowHarness&) = delete;
  ~FlowHarness();

  // Pushes |state| and applies it (the runtime's boot).
  void Start(std::string_view state) SE_REQUIRES(g_main)
      SE_EXCLUDES(g_sim_owner);
  // Runs the next tick with |frame| (its tick is set to the next one).
  TickResult Tick(InputFrame frame) SE_REQUIRES(g_main)
      SE_EXCLUDES(g_sim_owner);
  TickResult Idle() SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner);
  TickResult Press(rampage::Action action) SE_REQUIRES(g_main)
      SE_EXCLUDES(g_sim_owner);
  // Applies the commands queued by the last tick, as the next tick's
  // boundary would.
  void ApplyPending() SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner);

  // The stack's ids, bottom to top.
  [[nodiscard]] std::vector<std::string> Ids() const;
  [[nodiscard]] GameStateStack& States() noexcept { return states_; }
  [[nodiscard]] const ModuleInstance& Module() const noexcept {
    return module_;
  }
  [[nodiscard]] se::Tick NextTick() const noexcept {
    return driver_.NextTick();
  }

 private:
  ModuleInstance module_;
  std::unique_ptr<CachedContentSource> content_;
  GameStateStack states_;
  jobs::ForkJoinPool pool_;
  SimDriver driver_;
  SessionLog log_;
  ReplayRecorder recorder_;
  NullAudioSink audio_;
};

struct FlowSessionOptions {
  std::string start_state = std::string(rampage::kTitleState);
  std::string gameplay_scene = std::string(kDoomedScene);
  // Display rate of the synthetic clock; 0 runs one tick per frame.
  double hz = 0.0;
  double jitter_sec = 0.0;
  std::uint64_t clock_seed = 7;
  // The World seed (StateEnv).
  std::uint64_t seed = 42;
  Tick ticks = 0;
  InputSource* input = nullptr;
  // Record a replay (its start bytes: the Gameplay scene).
  bool record = false;
};

struct FlowSessionResult {
  // hashes[i], tops[i] and depths[i] are of tick i + 1: the hash after it,
  // the top state it ran in, the stack's depth after it.
  std::vector<std::uint64_t> hashes;
  std::vector<std::string> tops;
  std::vector<std::size_t> depths;
  std::vector<int> steps_per_frame;
  std::vector<std::byte> replay;
  std::string error;
};

[[nodiscard]] FlowSessionResult RunFlowSession(
    const FlowSessionOptions& options) SE_REQUIRES(g_main)
    SE_EXCLUDES(g_sim_owner);

// The flow's recorded session (tests/fixtures/flow/flow.seinput, written by
// tests/rampage/flow_input_gen.cc): the frames StampedInput latches over
// |ticks| ticks at 120 Hz from the session's scripted edges.
[[nodiscard]] std::vector<InputFrame> FlowScriptFrames(Tick ticks);
// The script's length in ticks.
inline constexpr Tick kFlowScriptTicks = 360;

// |frames| in the .seinput format (seinput.h), one line per tick with an
// edge, and the last tick.
[[nodiscard]] std::string WriteSeinput(const std::vector<InputFrame>& frames);

}  // namespace se::testing

#endif  // TESTS_RAMPAGE_SUPPORT_FLOW_SESSION_H_
