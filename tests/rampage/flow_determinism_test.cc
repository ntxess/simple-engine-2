// Copyright 2026 The simple-engine-2 Authors.
//
// determinism_rampage_flow (M13b): the recorded session of
// tests/fixtures/flow/flow.seinput (written by flow_input_gen.cc through
// StampedInput) runs from the Title through Gameplay, Pause, Gameplay, the
// player's death on tests/fixtures/flow/doomed.scene.json, GameOver, a
// retry, Pause and back to the Title. Under 30, 60, 144 and 160 Hz synthetic
// clocks and with the CRT's FMA3 paths on and off the per-tick hashes are
// identical, the transition ticks included, and they match the
// per-platform golden determinism_rampage_flow. A recorded run replays at
// another rate.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "game_rampage/flow.h"
#include "rampage/support/flow_session.h"
#include "rampage/support/seinput.h"
#include "se_core/fp.h"
#include "se_core/input_frame.h"
#include "se_sim/replay.h"
#include "support/golden.h"
#include "support/test_roles.h"

namespace se {
namespace {

// A state run: the state and the first tick that ran in it.
struct Run {
  std::string state;
  Tick first = 0;

  friend bool operator==(const Run&, const Run&) = default;
};

std::vector<Run> Runs(const std::vector<std::string>& tops) {
  std::vector<Run> runs;
  for (std::size_t i = 0; i < tops.size(); ++i) {
    if (runs.empty() || runs.back().state != tops[i]) {
      runs.push_back({tops[i], static_cast<Tick>(i + 1)});
    }
  }
  return runs;
}

std::vector<InputFrame> FlowFrames() {
  auto frames = testing::ReadSeinput(testing::FlowInputPath());
  REQUIRE(frames.has_value());
  return std::move(*frames);
}

testing::FlowSessionResult RunFlow(double hz, InputSource* input)
    SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner) {
  testing::FlowSessionOptions options;
  options.hz = hz;
  options.jitter_sec = hz > 0.0 ? 1.5e-3 : 0.0;
  options.clock_seed = 11;
  options.ticks = testing::kFlowScriptTicks;
  options.input = input;
  testing::FlowSessionResult result = testing::RunFlowSession(options);
  REQUIRE(result.error.empty());
  REQUIRE(result.hashes.size() == testing::kFlowScriptTicks);
  return result;
}

TEST_CASE("The flow's recorded session is its generator's output",
          "[rampage][flow]") {
  const std::vector<InputFrame> file = FlowFrames();
  const std::vector<InputFrame> script =
      testing::FlowScriptFrames(testing::kFlowScriptTicks);
  REQUIRE(file.size() == script.size());
  for (std::size_t i = 0; i < file.size(); ++i) {
    INFO("tick " << i + 1);
    CHECK(file[i].tick == script[i].tick);
    CHECK(file[i].held == script[i].held);
    CHECK(file[i].pressed == script[i].pressed);
    CHECK(file[i].released == script[i].released);
  }
}

TEST_CASE("determinism_rampage_flow: every state, independent of the clock",
          "[rampage][determinism][golden]") {
  const testing::MainThreadScope main_thread;
  testing::FrameListInput baseline_input(FlowFrames());
  const testing::FlowSessionResult baseline = RunFlow(0.0, &baseline_input);

  // Title, Gameplay, Pause, Gameplay, the death and GameOver, the retry,
  // Pause and the Title again, each at the tick after its key.
  const std::vector<Run> runs = Runs(baseline.tops);
  REQUIRE(runs.size() == 8);
  CHECK(runs[0] == Run{"title", 1});
  CHECK(runs[1] == Run{"gameplay", 21});
  CHECK(runs[2] == Run{"pause", 51});
  CHECK(runs[3] == Run{"gameplay", 81});
  REQUIRE(runs[4].state == "game_over");
  REQUIRE(runs[4].first > 81);
  REQUIRE(runs[4].first < 240);
  CHECK(runs[5] == Run{"gameplay", 241});
  CHECK(runs[6] == Run{"pause", 281});
  CHECK(runs[7] == Run{"title", 311});
  REQUIRE(baseline.depths[60] == 2);
  REQUIRE(baseline.depths.back() == 1);
  // The Gameplay World moves every tick it runs.
  for (std::size_t i = 21; i < 50; ++i) {
    CHECK(baseline.hashes[i] != baseline.hashes[i - 1]);
  }

  for (const double hz : {30.0, 60.0, 144.0, 160.0}) {
    INFO("hz = " << hz);
    testing::FrameListInput input(FlowFrames());
    const testing::FlowSessionResult run = RunFlow(hz, &input);
    CHECK(run.hashes == baseline.hashes);
    CHECK(run.tops == baseline.tops);
    const int max_steps = *std::max_element(run.steps_per_frame.begin(),
                                            run.steps_per_frame.end());
    if (hz == 30.0) REQUIRE(max_steps == 4);
    if (hz == 60.0) REQUIRE(max_steps == 2);
  }

  for (const bool fma3 : {true, false}) {
    INFO("fma3 = " << fma3);
    fp::SetFma3ForTest(fma3);
    testing::FrameListInput input(FlowFrames());
    const testing::FlowSessionResult run = RunFlow(144.0, &input);
    CHECK(run.hashes == baseline.hashes);
  }
  fp::ConfigureProcess();

  const testing::GoldenResult golden =
      testing::CheckGolden("determinism_rampage_flow", baseline.hashes);
  INFO(golden.message);
  CHECK(golden.ok);
}

TEST_CASE("A recorded flow session replays at another rate",
          "[rampage][flow][determinism]") {
  const testing::MainThreadScope main_thread;
  testing::FrameListInput input(FlowFrames());
  testing::FlowSessionOptions record;
  record.hz = 144.0;
  record.jitter_sec = 1e-3;
  record.ticks = testing::kFlowScriptTicks;
  record.input = &input;
  record.record = true;
  const testing::FlowSessionResult recorded = testing::RunFlowSession(record);
  REQUIRE(recorded.error.empty());
  auto reader = ReplayReader::Open(recorded.replay);
  REQUIRE(reader.has_value());
  REQUIRE(reader->Frames().size() == testing::kFlowScriptTicks);
  testing::FlowSessionOptions replay = record;
  replay.hz = 30.0;
  replay.record = false;
  replay.input = &*reader;
  const testing::FlowSessionResult replayed = testing::RunFlowSession(replay);
  CHECK(replayed.hashes == recorded.hashes);
  CHECK(replayed.tops == recorded.tops);
  CHECK(std::vector<std::uint64_t>(reader->Hashes().begin(),
                                   reader->Hashes().end()) == recorded.hashes);
}

}  // namespace
}  // namespace se
