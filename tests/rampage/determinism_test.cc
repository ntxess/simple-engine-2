// Copyright 2026 The simple-engine-2 Authors.
//
// The sandbox scene under 30, 60, 144 and 160 Hz synthetic frame clocks
// with jitter gives the per-tick hashes of one tick per frame, for 3600
// ticks of scripted input, and they match the per-platform golden
// determinism_rampage. The legacy effects scenario (deaths, the watchdog,
// every effect kind) is clock-independent too, a recorded session replays
// to the same hashes at another rate, and the CRT's FMA3 paths do not
// change them.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "rampage/support/rampage_session.h"
#include "rampage/support/seinput.h"
#include "se_core/fp.h"
#include "se_core/world.h"
#include "se_sim/replay.h"
#include "support/golden.h"
#include "support/test_roles.h"

namespace se {
namespace {

constexpr Tick kTicks = 3600;

testing::RampageSessionOptions Options(double hz) {
  testing::RampageSessionOptions options;
  options.scene = testing::SandboxSceneRequest();
  options.hz = hz;
  options.jitter_sec = hz > 0.0 ? 1.5e-3 : 0.0;
  options.clock_seed = 11;
  options.ticks = kTicks;
  return options;
}

testing::RampageSessionResult Run(const testing::RampageSessionOptions& options)
    SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner) {
  testing::RampageSessionResult result = testing::RunRampageSession(options);
  REQUIRE(result.error.empty());
  REQUIRE(result.hashes.size() == options.ticks);
  return result;
}

// What a session does: entities at the start and the end, and the ticks
// with an effect running.
struct Activity {
  std::size_t first_count = 0;
  std::size_t last_count = 0;
  std::set<Tick> effect_ticks;
};

testing::RampageSessionResult RunWatched(testing::RampageSessionOptions options,
                                         Activity& activity) SE_REQUIRES(g_main)
    SE_EXCLUDES(g_sim_owner) {
  options.on_world = [&activity](const World& world) {
    if (world.Clock().tick == 0) activity.first_count = world.EntityCount();
    activity.last_count = world.EntityCount();
    for (const auto [e, active] :
         world.CReg().view<const rampage::ActiveEffects>().each()) {
      if (!active.effects.empty()) {
        activity.effect_ticks.insert(world.Clock().tick);
      }
    }
  };
  return Run(options);
}

// The hashes at 30, 60, 144 and 160 Hz equal |baseline|, and the clocks
// really group the ticks differently.
void CheckClocks(const testing::RampageSessionOptions& base,
                 const std::vector<std::uint64_t>& baseline) SE_REQUIRES(g_main)
    SE_EXCLUDES(g_sim_owner) {
  for (const double hz : {30.0, 60.0, 144.0, 160.0}) {
    INFO("hz = " << hz);
    testing::RampageSessionOptions options = base;
    options.hz = hz;
    options.jitter_sec = 1.5e-3;
    const testing::RampageSessionResult run = Run(options);
    CHECK(run.hashes == baseline);
    int max_steps = 0;
    int zero_frames = 0;
    for (const int steps : run.steps_per_frame) {
      max_steps = std::max(max_steps, steps);
      if (steps == 0) ++zero_frames;
    }
    if (hz == 30.0) REQUIRE(max_steps == 4);
    if (hz == 60.0) REQUIRE(max_steps == 2);
    if (hz >= 144.0) REQUIRE(zero_frames > 0);
  }
}

TEST_CASE("Sandbox hashes are independent of the frame clock",
          "[rampage][determinism][golden]") {
  const testing::MainThreadScope main_thread;
  // The player walks the arena and hits the enemies it meets.
  Activity activity;
  const testing::RampageSessionResult baseline =
      RunWatched(Options(0.0), activity);
  REQUIRE(activity.first_count == 17);
  REQUIRE(activity.effect_ticks.size() > 100);
  for (std::size_t i = 1; i < baseline.hashes.size(); ++i) {
    REQUIRE(baseline.hashes[i] != baseline.hashes[i - 1]);
  }
  CheckClocks(Options(0.0), baseline.hashes);

  const testing::GoldenResult golden =
      testing::CheckGolden("determinism_rampage", baseline.hashes);
  INFO(golden.message);
  CHECK(golden.ok);
}

// The legacy effects scenario adds what the sandbox does not reach in 3600
// ticks: deaths, the watchdog, every effect kind and both source kinds.
TEST_CASE("Effects scenario hashes are independent of the frame clock",
          "[rampage][determinism]") {
  const testing::MainThreadScope main_thread;
  auto frames = testing::ReadSeinput(testing::RampageFixtureDir() +
                                     "/reference/effects.seinput");
  REQUIRE(frames.has_value());
  testing::FrameListInput input(std::move(*frames));
  testing::RampageSessionOptions base = Options(0.0);
  base.scene = testing::ReferenceScenarioRequest("effects");
  base.input = &input;
  Activity activity;
  const testing::RampageSessionResult baseline = RunWatched(base, activity);
  REQUIRE(activity.first_count == 13);
  CHECK(activity.last_count < activity.first_count);
  REQUIRE(activity.effect_ticks.size() > 1000);
  CheckClocks(base, baseline.hashes);
}

TEST_CASE("A recorded sandbox session replays at another rate",
          "[rampage][determinism]") {
  const testing::MainThreadScope main_thread;
  testing::RampageSessionOptions record = Options(144.0);
  record.ticks = 900;
  record.record = true;
  const testing::RampageSessionResult recorded = Run(record);
  auto reader = ReplayReader::Open(recorded.replay);
  REQUIRE(reader.has_value());
  REQUIRE(reader->Frames().size() == 900);
  REQUIRE(reader->Hashes().size() == 900);
  testing::RampageSessionOptions replay = Options(30.0);
  replay.ticks = 900;
  replay.input = &*reader;
  const testing::RampageSessionResult replayed = Run(replay);
  CHECK(replayed.hashes == recorded.hashes);
  CHECK(std::vector<std::uint64_t>(reader->Hashes().begin(),
                                   reader->Hashes().end()) == recorded.hashes);
}

TEST_CASE("Sandbox hashes do not change with the CRT's FMA3 paths",
          "[rampage][determinism]") {
  const testing::MainThreadScope main_thread;
  testing::RampageSessionOptions options = Options(0.0);
  options.ticks = 1200;
  fp::SetFma3ForTest(true);
  const testing::RampageSessionResult on = Run(options);
  fp::SetFma3ForTest(false);
  const testing::RampageSessionResult off = Run(options);
  fp::ConfigureProcess();
  CHECK(on.hashes == off.hashes);
}

}  // namespace
}  // namespace se
