// Copyright 2026 The simple-engine-2 Authors.
//
// AppConfig's command line, the M5 frame clock and BasicInputService.

#include <SDL3/SDL_scancode.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "se_core/input_frame.h"
#include "se_core/time.h"
#include "se_runtime/app_config.h"
#include "se_runtime/basic_input.h"
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
  // An unknown refresh rate counts as 60 Hz.
  const FrameClock unknown = source.Next(1'020'000'000, 0.0, false);
  CHECK(unknown.period_sec > 0.0166);
  CHECK(unknown.period_sec < 0.0167);
  // After a reset the next frame is a first frame: no catch-up.
  source.Reset();
  const FrameClock after = source.Next(9'000'000'000, 60.0, false);
  REQUIRE(after.prev_frame_start_ns == 0);
  REQUIRE(source.Resets() == 1);
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

TEST_CASE("BasicInputService latches held keys per tick", "[basic_input]") {
  constexpr std::array<ActionBinding, 4> kBindings = {{
      {"up", "W"},
      {"enter", "Enter"},
      {"back", "Enter"},
      {"broken", "NoSuchKey"},
  }};
  BasicInputService input(ActionMapDefaults{.bindings = kBindings});
  REQUIRE(input.UnknownKeys() == std::vector<std::string>{"NoSuchKey"});
  REQUIRE(input.ActionsFor(SDL_SCANCODE_RETURN) == 0x6);

  input.OnKey(SDL_SCANCODE_W, true, false);
  input.OnKey(SDL_SCANCODE_W, true, true);  // a repeat
  const InputFrame first = input.Latch(1, 0, true);
  REQUIRE(first.tick == 1);
  REQUIRE(first.held == 0x1);
  REQUIRE(first.pressed[0] == 1);
  CHECK(first.platform_live);
  const InputFrame second = input.Latch(2, 0, true);
  REQUIRE(second.held == 0x1);
  REQUIRE(second.pressed[0] == 0);

  // A tap between two latches shows as a press and a release.
  input.OnKey(SDL_SCANCODE_RETURN, true, false);
  input.OnKey(SDL_SCANCODE_RETURN, false, false);
  const InputFrame third = input.Latch(3, 0, true);
  REQUIRE(third.held == 0x1);
  REQUIRE(third.pressed[1] == 1);
  REQUIRE(third.released[1] == 1);
  REQUIRE(third.pressed[2] == 1);

  // Focus lost: everything held is released.
  input.ClearHeld();
  const InputFrame fourth = input.Latch(4, 0, true);
  REQUIRE(fourth.held == 0);
  REQUIRE(fourth.released[0] == 1);
}

}  // namespace
}  // namespace se
