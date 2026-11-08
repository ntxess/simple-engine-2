// Copyright 2026 The simple-engine-2 Authors.
//
// The M2 determinism exit: per-tick hash sequences of the kernel test
// module, including the GameOver transition at tick 300 and the logged edits
// at ticks 100 and 250, are identical under 30, 60, 144 and 160 Hz synthetic
// frame clocks with jitter, match the golden for this platform and compiler,
// and do not change with the CRT's FMA3 paths forced on or off.
//
// Two further cases close what the regular clocks cannot show:
//   - irregular clocks (missed vblanks, unknown vblank counts, jitter beyond
//     the snap window) regroup the ticks into frames, with debt and
//     dilation, and the hashes still do not change;
//   - with input from wall-clock-stamped edges latched by the stepper's tick
//     windows (StampedInput), the InputFrames themselves are the same at
//     every regular clock, and a replay carries them across an irregular
//     one.

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "se_core/fp.h"
#include "se_core/input_frame.h"
#include "se_sim/replay.h"
#include "se_sim/synthetic_clock.h"
#include "support/golden.h"
#include "support/kernel_session.h"
#include "support/kernel_test_module.h"
#include "support/stamped_input.h"
#include "support/test_roles.h"

namespace se {
namespace {

constexpr Tick kTicks = 600;

testing::KernelSessionOptions Options(double hz) {
  testing::KernelSessionOptions options;
  options.hz = hz;
  options.jitter_sec = hz > 0.0 ? 1.5e-3 : 0.0;
  options.clock_seed = 11;
  options.ticks = kTicks;
  options.edits_path = testing::EditsFixturePath();
  return options;
}

testing::KernelSessionResult Run(const testing::KernelSessionOptions& options)
    SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner) {
  testing::KernelSessionResult result = testing::RunKernelSession(options);
  REQUIRE(result.error.empty());
  REQUIRE(result.hashes.size() == options.ticks);
  return result;
}

testing::KernelSessionResult Run(double hz) SE_REQUIRES(g_main)
    SE_EXCLUDES(g_sim_owner) {
  return Run(Options(hz));
}

TEST_CASE("Per-tick hashes are independent of the frame clock",
          "[determinism][golden]") {
  testing::MainThreadScope main_thread;
  const testing::KernelSessionResult baseline = Run(0.0);

  // The scenario covers what it claims to.
  REQUIRE(baseline.session_applied == 4);
  REQUIRE(baseline.session_failures == 0);
  CHECK(baseline.top_states[testing::kGameOverTick - 1] ==
        testing::kGameplayState);
  CHECK(baseline.top_states[testing::kGameOverTick] == testing::kGameOverState);
  // Every tick changes the state.
  for (std::size_t i = 1; i < baseline.hashes.size(); ++i) {
    REQUIRE(baseline.hashes[i] != baseline.hashes[i - 1]);
  }

  for (const double hz : {30.0, 60.0, 144.0, 160.0}) {
    INFO("hz = " << hz);
    const testing::KernelSessionResult run = Run(hz);
    CHECK(run.hashes == baseline.hashes);
    CHECK(run.top_states == baseline.top_states);
    int max_steps = 0;
    int zero_frames = 0;
    for (const int steps : run.steps_per_frame) {
      max_steps = std::max(max_steps, steps);
      if (steps == 0) ++zero_frames;
    }
    // The clocks really differ: 4 ticks per frame at 30 Hz, frames without
    // a tick at 144 and 160 Hz.
    if (hz == 30.0) REQUIRE(max_steps == 4);
    if (hz == 60.0) REQUIRE(max_steps == 2);
    if (hz >= 144.0) REQUIRE(zero_frames > 0);
  }

  const testing::GoldenResult golden =
      testing::CheckGolden("determinism_kernel", baseline.hashes);
  INFO(golden.message);
  CHECK(golden.ok);
}

struct IrregularClock {
  const char* name;
  double hz;
  double jitter_sec;
  double miss_probability;
  bool vblanks_known;
  double timer_p99_sec;
};

TEST_CASE("Per-tick hashes are independent of irregular frame grouping",
          "[determinism]") {
  testing::MainThreadScope main_thread;
  const testing::KernelSessionResult baseline = Run(0.0);
  const std::vector<IrregularClock> clocks = {
      // Locked: 4 or 6 ticks due after a miss, at most k_rt + 1 = 3 run,
      // the rest is debt or dilation.
      {"60 Hz, missed vblanks", 60.0, 1.5e-3, 0.15, true, -1.0},
      // No vblank counts: the stepper rounds the delta to intervals.
      {"60 Hz, missed vblanks, counts unknown", 60.0, 1.5e-3, 0.15, false,
       -1.0},
      // Free: 3 ms of jitter against a 1 ms snap window, so nearly half the
      // deltas do not snap and the accumulator takes them raw.
      {"144 Hz, unsnapped deltas", 144.0, 3.0e-3, 0.0, true, 0.0},
      {"160 Hz, unsnapped deltas and misses", 160.0, 3.0e-3, 0.1, false, 0.0},
      {"30 Hz, missed vblanks", 30.0, 1.5e-3, 0.1, true, -1.0},
  };
  for (const IrregularClock& c : clocks) {
    INFO(c.name);
    testing::KernelSessionOptions options = Options(c.hz);
    options.jitter_sec = c.jitter_sec;
    options.miss_probability = c.miss_probability;
    options.vblanks_known = c.vblanks_known;
    options.timer_p99_sec = c.timer_p99_sec;
    const testing::KernelSessionResult run = Run(options);
    CHECK(run.hashes == baseline.hashes);
    CHECK(run.top_states == baseline.top_states);

    // The grouping really changed: not the regular clock's frames, and more
    // than one tick count per frame.
    const testing::KernelSessionResult regular = Run(c.hz);
    CHECK(run.steps_per_frame != regular.steps_per_frame);
    const std::set<int> counts(run.steps_per_frame.begin(),
                               run.steps_per_frame.end());
    REQUIRE(counts.size() >= 2);
    if (c.miss_probability > 0.0) {
      // Catch-up frames: more ticks than the regular clock ever runs.
      const int regular_max = *std::max_element(regular.steps_per_frame.begin(),
                                                regular.steps_per_frame.end());
      CHECK(*counts.rbegin() > regular_max);
    }
  }
}

// Latches |inner| with every window ending |shift_ns| late: the input a
// stepper whose windows were that far off would feed the simulation.
class ShiftedWindows final : public InputSource {
 public:
  ShiftedWindows(InputSource& inner, std::uint64_t shift_ns)
      : inner_(&inner), shift_ns_(shift_ns) {}
  InputFrame Latch(Tick tick, std::uint64_t tick_window_end_ns,
                   bool last_tick_of_frame) override {
    return inner_->Latch(tick, tick_window_end_ns + shift_ns_,
                         last_tick_of_frame);
  }

 private:
  InputSource* inner_;
  std::uint64_t shift_ns_;
};

struct StampedRun {
  testing::KernelSessionResult result;
  std::vector<InputFrame> frames;
  std::uint64_t edges = 0;
};

// A recorded run with StampedInput (optionally wrapped).
StampedRun RunStamped(testing::KernelSessionOptions options,
                      std::uint64_t shift_ns = 0) SE_REQUIRES(g_main)
    SE_EXCLUDES(g_sim_owner) {
  testing::StampedInput stamped(5, SyntheticFrameClock::kOriginNs);
  ShiftedWindows shifted(stamped, shift_ns);
  options.input = &shifted;
  options.record = true;
  StampedRun run;
  run.result = Run(options);
  run.edges = stamped.Delivered();
  auto reader = ReplayReader::Open(run.result.replay);
  REQUIRE(reader.has_value());
  run.frames.assign(reader->Frames().begin(), reader->Frames().end());
  return run;
}

TEST_CASE(
    "Edges latched by tick windows give the same InputFrames at every "
    "clock",
    "[determinism]") {
  testing::MainThreadScope main_thread;
  // 120 Hz without jitter: one tick per frame, each window ending exactly
  // on the tick grid.
  testing::KernelSessionOptions grid = Options(120.0);
  grid.jitter_sec = 0.0;
  const StampedRun baseline = RunStamped(grid);
  REQUIRE(baseline.edges >= 50);
  std::size_t frames_with_edges = 0;
  for (const InputFrame& frame : baseline.frames) {
    if (frame.edge_count > 0) ++frames_with_edges;
  }
  REQUIRE(frames_with_edges >= 50);
  // Stamped input drives the scene: not the scripted run's hashes.
  CHECK(baseline.result.hashes != Run(0.0).hashes);

  for (const double hz : {30.0, 60.0, 144.0, 160.0}) {
    INFO("hz = " << hz);
    const StampedRun run = RunStamped(Options(hz));
    CHECK(run.frames == baseline.frames);
    CHECK(run.result.hashes == baseline.result.hashes);
  }

  // The control: windows half a tick off move edges into other ticks, and
  // the check above sees it.
  const StampedRun shifted = RunStamped(Options(60.0), 4'166'667);
  CHECK(shifted.frames != baseline.frames);
  CHECK(shifted.result.hashes != baseline.result.hashes);

  // Missed vblanks compress a frame's windows, so edges may land in another
  // tick; the replay records where they landed and reproduces the run at
  // any other clock.
  testing::KernelSessionOptions irregular = Options(60.0);
  irregular.miss_probability = 0.15;
  const StampedRun recorded = RunStamped(irregular);
  auto reader = ReplayReader::Open(recorded.result.replay);
  REQUIRE(reader.has_value());
  testing::KernelSessionOptions replay = Options(144.0);
  replay.edits_path.clear();  // the replay carries the session log
  replay.replay = &*reader;
  const testing::KernelSessionResult replayed = Run(replay);
  CHECK(replayed.hashes == recorded.result.hashes);
}

// CRT transcendentals over a range of arguments, folded into a hash.
std::uint64_t CrtMathHash() {
  std::uint64_t hash = 0xCBF29CE484222325ULL;
  for (int i = 0; i < 20000; ++i) {
    const double x = -50.0 + static_cast<double>(i) * 0.00731;
    const double values[] = {std::sin(x),
                             std::cos(x),
                             std::exp(x * 0.01),
                             std::log(std::abs(x) + 1.0),
                             std::pow(std::abs(x) + 0.5, 1.37),
                             std::atan2(x, 3.0)};
    for (const double v : values) {
      hash = (hash ^ std::bit_cast<std::uint64_t>(v)) * 0x100000001B3ULL;
    }
  }
  return hash;
}

TEST_CASE("FMA3 on and off give the same hashes", "[determinism][golden]") {
  testing::MainThreadScope main_thread;
  if (!fp::SetFma3ForTest(true)) {
    SUCCEED("no FMA3 switch on this platform");
    return;
  }
  const testing::KernelSessionResult with_fma = Run(60.0);
  REQUIRE(fp::SetFma3ForTest(false));
  const testing::KernelSessionResult without_fma = Run(144.0);
  fp::ConfigureProcess();
  CHECK(with_fma.hashes == without_fma.hashes);
  const testing::GoldenResult golden =
      testing::CheckGolden("determinism_kernel", with_fma.hashes);
  INFO(golden.message);
  CHECK(golden.ok);

  // The control: on an FMA3 CPU the same switch changes the CRT's
  // transcendentals, so the check above fails as soon as simulation code
  // calls them instead of se::dm (the kernel module calls none).
  if ((fp::CpuFeatureBits() & fp::kCpuFma3) != 0) {
    REQUIRE(fp::SetFma3ForTest(true));
    const std::uint64_t crt_with_fma = CrtMathHash();
    REQUIRE(fp::SetFma3ForTest(false));
    const std::uint64_t crt_without_fma = CrtMathHash();
    fp::ConfigureProcess();
    CHECK(crt_with_fma != crt_without_fma);
  }
}

TEST_CASE("The det-crosscheck finds no divergence", "[determinism]") {
  testing::MainThreadScope main_thread;
  testing::KernelSessionOptions options = Options(144.0);
  options.det_crosscheck = 7;
  const testing::KernelSessionResult checked =
      testing::RunKernelSession(options);
  REQUIRE(checked.error.empty());
  INFO(checked.crosscheck_report);
  CHECK(checked.crosscheck_ok);
  REQUIRE(checked.crosschecks > 50);
  // Cross-checking does not change the run.
  CHECK(checked.hashes == Run(0.0).hashes);
}

}  // namespace
}  // namespace se
