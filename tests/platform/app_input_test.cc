// Copyright 2026 The simple-engine-2 Authors.
//
// The input sampler in the App (label unit, headless, everywhere): it
// starts at the first iterate, inside ADR-047's window, polls while the app
// runs, and has stopped and been joined when the app has shut down; an SDL
// virtual gamepad is opened by the App and its presses reach the ticks
// with the sampler as their source; an unknown key in the settings file
// fails the boot.

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_timer.h>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "platform/support/test_app.h"
#include "se_core/input_frame.h"
#include "se_platform/fs.h"
#include "se_platform/input_sampler.h"
#include "se_runtime/app.h"
#include "se_runtime/integration.h"
#include "se_sim/replay.h"
#include "support/test_roles.h"

#ifndef SE_TEST_OUT_DIR
#error "SE_TEST_OUT_DIR must name a writable directory"
#endif

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

TEST_CASE("A virtual gamepad reaches the App's ticks through the sampler",
          "[app][input_sampler]") {
  GameSetup setup = testing::TestGameSetup();
  SDL_JoystickID id = 0;
  SDL_Joystick* joystick = nullptr;
  std::size_t open_pads = 0;
  setup.on_boot = [&id](App& /*app*/) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
    desc.name = "se virtual gamepad (app)";
    id = SDL_AttachVirtualJoystick(&desc);
  };
  setup.on_frame = [&](App& app, const FrameReport& report) {
    if (report.iterate == 5) {
      joystick = SDL_OpenJoystick(id);
      if (joystick != nullptr) {
        SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                                     true);
      }
    }
    if (report.iterate == 40 && joystick != nullptr) {
      SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                                   false);
    }
    if (report.iterate == 60) open_pads = app.OpenGamepads();
    // A headless App does not wait for its frames: 2 ms of real time per
    // iterate lets the 1 kHz sampler poll between them.
    SDL_DelayNS(2'000'000);
  };
  setup.on_shutdown = [&joystick, &id](App& /*app*/, SDL_AppResult) {
    if (joystick != nullptr) SDL_CloseJoystick(joystick);
    if (id != 0) SDL_DetachVirtualJoystick(id);
  };
  const std::string replay =
      std::string(SE_TEST_OUT_DIR) + "/virtual_gamepad.sereplay";
  REQUIRE(testing::RunTestApp(std::move(setup), {"--headless", "--frames", "90",
                                                 "--fixed-frame-dt", "8.3333",
                                                 "--record", replay}) == 0);
  REQUIRE(id != 0);
  REQUIRE(open_pads == 1);
  auto bytes = fs::ReadFile(replay);
  REQUIRE(bytes);
  auto reader = ReplayReader::Open(*bytes);
  REQUIRE(reader);
  std::vector<InputEdge> edges;
  for (const InputFrame& frame : reader->Frames()) {
    for (std::uint8_t i = 0; i < frame.edge_count; ++i) {
      edges.push_back(frame.edges[i]);
    }
  }
  REQUIRE(edges.size() == 2);
  // Action 0 is "left": the d-pad's left in the test module.
  REQUIRE(edges[0].action == 0);
  REQUIRE(edges[0].down);
  REQUIRE_FALSE(edges[1].down);
  REQUIRE(edges[0].source == EdgeSource::kGamepadSampler);
  REQUIRE(edges[1].source == EdgeSource::kGamepadSampler);
}

TEST_CASE("An unknown key in the settings file fails the boot",
          "[app][action_map]") {
  const std::string root = std::string(SE_TEST_OUT_DIR) + "/bad_settings";
  REQUIRE(fs::CreateDirectories(root));
  const std::string text =
      R"({"control-settings": {"key-bindings": {"key-left": "nosuchkey"}}})";
  const auto bytes = std::as_bytes(std::span<const char>(text));
  REQUIRE(fs::WriteFile(root + "/settings.json",
                        std::vector<std::byte>(bytes.begin(), bytes.end())));
  GameSetup setup = testing::TestGameSetup();
  setup.settings_file = "settings.json";
  bool booted = false;
  setup.on_boot = [&booted](App& /*app*/) { booted = true; };
  REQUIRE(testing::RunTestApp(std::move(setup), {"--headless", "--frames", "5",
                                                 "--content-root", root}) == 1);
  REQUIRE_FALSE(booted);
  REQUIRE(integration::EngineThreadsRunning() == 0);
}

}  // namespace
}  // namespace se
