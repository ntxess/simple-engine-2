// Copyright 2026 The simple-engine-2 Authors.
//
// The input sampler in the App (label unit, headless, everywhere): it
// starts at the first iterate, inside ADR-047's window, polls while the app
// runs, and has stopped and been joined when the app has shut down.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>

#include "platform/support/test_app.h"
#include "se_platform/input_sampler.h"
#include "se_runtime/app.h"
#include "se_runtime/integration.h"
#include "support/test_roles.h"

namespace se {
namespace {

TEST_CASE("The App runs the input sampler from its first iterate",
          "[app][input_sampler]") {
  GameSetup setup = testing::TestGameSetup();
  bool at_boot = true;
  std::uint64_t first_iterate_polls = 0;
  std::uint64_t polls = 0;
  bool running_at_shutdown = false;
  setup.on_boot = [&at_boot](App& app) {
    // Boot (SDL_AppInit) starts no event-pushing thread.
    at_boot = app.Sampler() == nullptr;
  };
  setup.on_frame = [&](App& app, const FrameReport& report) {
    const InputSampler* sampler = app.Sampler();
    REQUIRE(sampler != nullptr);
    if (report.iterate == 1) first_iterate_polls = sampler->Stats().polls;
    polls = sampler->Stats().polls;
  };
  setup.on_shutdown = [&running_at_shutdown](App& app, SDL_AppResult) {
    running_at_shutdown = app.Sampler() != nullptr && app.Sampler()->Running();
  };
  REQUIRE(testing::RunTestApp(
              std::move(setup),
              {"--headless", "--frames", "60", "--fixed-frame-dt", "5"}) == 0);
  REQUIRE(at_boot);
  REQUIRE(running_at_shutdown);
  REQUIRE(polls >= first_iterate_polls);
  // Joined before shutdown step 4: nothing is left running.
  REQUIRE(integration::EngineThreadsRunning() == 0);
}

}  // namespace
}  // namespace se
