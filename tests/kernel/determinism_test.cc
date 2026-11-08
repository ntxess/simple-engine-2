// Copyright 2026 The simple-engine-2 Authors.
//
// The M2 determinism exit: per-tick hash sequences of the kernel test
// module, including the GameOver transition at tick 300 and the logged edits
// at ticks 100 and 250, are identical under 30, 60, 144 and 160 Hz synthetic
// frame clocks with jitter, match the golden for this platform and compiler,
// and do not change with the CRT's FMA3 paths forced on or off.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "se_core/fp.h"
#include "support/golden.h"
#include "support/kernel_session.h"
#include "support/kernel_test_module.h"
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

testing::KernelSessionResult Run(double hz) SE_REQUIRES(g_main)
    SE_EXCLUDES(g_sim_owner) {
  testing::KernelSessionResult result = testing::RunKernelSession(Options(hz));
  REQUIRE(result.error.empty());
  REQUIRE(result.hashes.size() == kTicks);
  return result;
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
