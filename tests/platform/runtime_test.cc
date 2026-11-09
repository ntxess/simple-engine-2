// Copyright 2026 The simple-engine-2 Authors.
//
// AppConfig's command line and the M5 frame clock.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "se_core/input_frame.h"
#include "se_core/time.h"
#include "se_runtime/app_config.h"
#include "se_runtime/frame_clock.h"
#include "se_sim/game_module.h"

namespace se {
namespace {

std::expected<AppConfig, Error> Parse(std::vector<const char*> args) {
  return ParseAppConfig(std::span<const char* const>(args));
}

TEST_CASE("The runtime's command line", "[app_config]") {
  auto config = Parse(
      {"--frames", "300", "--fixed-frame-dt=16.6667", "--gpu-driver", "vulkan",
       "--sim-driver", "inline", "--profile", "--headless", "--no-activate",
       "--return-on-quit", "--log-level", "debug", "--", "--scene", "x.json"});
  REQUIRE(config);
  REQUIRE(config->frames == 300);
  CHECK(config->fixed_frame_dt_ms > 16.666);
  CHECK(config->fixed_frame_dt_ms < 16.667);
  REQUIRE(config->gpu_driver == "vulkan");
  REQUIRE(config->sim_driver == "inline");
  CHECK(config->profile);
  CHECK(config->headless);
  CHECK_FALSE(config->activate_when_shown);
  CHECK_FALSE(config->terminate_on_quit);
  CHECK(config->log_level == LogLevel::kDebug);
  CHECK(config->game_args == std::vector<std::string>{"--scene", "x.json"});

  auto defaults = Parse({});
  REQUIRE(defaults);
  REQUIRE(defaults->frames == 0);
  CHECK(defaults->activate_when_shown);
  CHECK(defaults->terminate_on_quit == kReleaseBuild);
}

TEST_CASE("Bad command lines are errors", "[app_config]") {
  CHECK_FALSE(Parse({"--frames"}));
  CHECK_FALSE(Parse({"--frames", "-3"}));
  CHECK_FALSE(Parse({"--frames", "12x"}));
  CHECK_FALSE(Parse({"--fixed-frame-dt", "0"}));
  CHECK_FALSE(Parse({"--sim-driver", "threaded"}));
  CHECK_FALSE(Parse({"--headless=yes"}));
  CHECK_FALSE(Parse({"--log-level", "loud"}));
  auto unknown = Parse({"--no-such-option"});
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error().message.find("--no-such-option") != std::string::npos);
}

TEST_CASE("Test hooks parse where they are built in", "[app_config]") {
  auto config = Parse({"--fail-init-step", "window", "--inject-throw",
                       "event:3", "--inject-null-acquire", "10:30"});
  if (!kRuntimeTestHooks) {
    CHECK_FALSE(config);
    return;
  }
  REQUIRE(config);
  REQUIRE(config->fail_init_step == 6);
  CHECK(config->throw_site == ThrowSite::kEvent);
  REQUIRE(config->throw_on_call == 3);
  REQUIRE(config->null_acquire_from == 10);
  REQUIRE(config->null_acquire_count == 30);
  auto numbered = Parse({"--fail-init-step=10", "--inject-throw", "iterate"});
  REQUIRE(numbered);
  REQUIRE(numbered->fail_init_step == 10);
  CHECK(numbered->throw_site == ThrowSite::kIterate);
  REQUIRE(numbered->throw_on_call == 1);
  CHECK_FALSE(Parse({"--fail-init-step", "11"}));
  CHECK_FALSE(Parse({"--fail-init-step", "nothing"}));
  REQUIRE(InitStepNames().size() == 10);
  REQUIRE(InitStepNames().front() == "crash-handler");
  REQUIRE(InitStepNames().back() == "game");
}

TEST_CASE("The frame clock follows SDL's clock and the display period",
          "[frame_clock]") {
  FrameClockSource source;
  const FrameClock first = source.Next(1'000'000'000, 144.0, false);
  REQUIRE(first.prev_frame_start_ns == 0);
  CHECK(first.period_sec > 1.0 / 144.5);
  CHECK(first.period_sec < 1.0 / 143.5);
  CHECK_FALSE(first.vblanks_known);
  const FrameClock second = source.Next(1'007'000'000, 144.0, true);
  REQUIRE(second.prev_frame_start_ns == 1'000'000'000);
  CHECK(second.modal);
  CHECK(second.real_delta > 0.0069);
  CHECK_FALSE(source.UsingMeasuredPeriod());
  // After a reset the next frame is a first frame: no catch-up.
  source.Reset();
  const FrameClock after = source.Next(9'000'000'000, 144.0, false);
  REQUIRE(after.prev_frame_start_ns == 0);
  REQUIRE(source.Resets() == 1);
  // An unknown refresh rate counts as 60 Hz.
  FrameClockSource fresh;
  const FrameClock unknown = fresh.Next(1'000'000'000, 0.0, false);
  CHECK(unknown.period_sec > 0.0166);
  CHECK(unknown.period_sec < 0.0167);
}

TEST_CASE("Frames faster than the display mode say fall back to their rate",
          "[frame_clock]") {
  // SDL reports 60 Hz, but the frames present at 165 Hz (a window on
  // another monitor). With the display's period, Locked cadence would round
  // each 6 ms delta to no vblank and run no ticks.
  FrameClockSource source;
  FixedStepper stepper(120);
  constexpr std::uint64_t kFrameNs = 6'060'606;
  std::uint64_t now = 1'000'000'000;
  int ticks = 0;
  for (int i = 0; i < 330; ++i) {
    const FrameClock clock = source.Next(now, 60.0, false);
    ticks +=
        stepper.Plan(clock, FrameCosts{}, /*running=*/true, false, false).steps;
    now += kFrameNs;
  }
  CHECK(source.UsingMeasuredPeriod());
  CHECK(source.MeasuredPeriodSec() > 0.0060);
  CHECK(source.MeasuredPeriodSec() < 0.0061);
  // Two seconds of frames run about two seconds of ticks.
  REQUIRE(ticks >= 236);
  REQUIRE(ticks <= 244);
  // Frames that match the display mode use its period again.
  for (int i = 0; i < 100; ++i) {
    static_cast<void>(source.Next(now, 60.0, false));
    now += 16'666'667;
  }
  CHECK_FALSE(source.UsingMeasuredPeriod());
  // Slow iterates (Suspended runs at 10 Hz) are forgotten at a reset.
  for (int i = 0; i < 30; ++i) {
    static_cast<void>(source.Next(now, 60.0, false));
    now += 100'000'000;
  }
  CHECK(source.UsingMeasuredPeriod());
  source.Reset();
  const FrameClock resumed = source.Next(now, 60.0, false);
  CHECK_FALSE(source.UsingMeasuredPeriod());
  CHECK(resumed.period_sec < 0.0167);
}

TEST_CASE("A fixed frame dt makes the clock synthetic", "[frame_clock]") {
  FrameClockSource source(0.016);
  CHECK(source.Synthetic());
  const FrameClock a = source.Next(5'000'000'000, 144.0, false);
  const FrameClock b = source.Next(5'001'000'000, 144.0, false);
  const FrameClock c = source.Next(9'000'000'000, 144.0, false);
  REQUIRE(b.frame_start_ns - a.frame_start_ns == 16'000'000);
  REQUIRE(c.frame_start_ns - b.frame_start_ns == 16'000'000);
  CHECK(c.vblanks_known);
  REQUIRE(c.elapsed_vblanks == 1);
  // The stepper runs exactly 120 ticks per synthetic second.
  FixedStepper stepper(120);
  FrameClockSource clock(1.0 / 60.0);
  int ticks = 0;
  for (int i = 0; i < 61; ++i) {
    ticks += stepper
                 .Plan(clock.Next(1'000'000'000, 60.0, false), FrameCosts{},
                       /*running=*/true, false, false)
                 .steps;
  }
  REQUIRE(ticks == 122);
}

}  // namespace
}  // namespace se
