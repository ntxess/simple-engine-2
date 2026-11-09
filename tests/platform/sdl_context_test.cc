// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/sdl_context.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_log.h>

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/log.h"
#include "support/test_roles.h"

namespace se {
namespace {

std::vector<std::string>& Lines() {
  static auto* const kLines = new std::vector<std::string>();
  return *kLines;
}

void Capture(LogLevel /*level*/, std::string_view line) {
  Lines().emplace_back(line);
}

TEST_CASE("SdlContext initialises SDL and quits what it started", "[sdl]") {
  const testing::MainThreadScope main;
  {
    SdlInitConfig config;
    // The dummy driver exists on every platform, including the WSL builds
    // without window-system packages.
    config.video_driver = "dummy";
    config.app_name = "se_platform_tests";
    auto sdl = SdlContext::Init(config);
    REQUIRE(sdl);
    REQUIRE(sdl->VideoDriver() == "dummy");
    REQUIRE(SDL_WasInit(SDL_INIT_VIDEO) != 0);
    REQUIRE(SDL_WasInit(SDL_INIT_GAMEPAD) != 0);
    CHECK(CheckMainThreadMatchesSdl());
    const char* auto_update = SDL_GetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS);
    REQUIRE(auto_update != nullptr);
    REQUIRE(std::string_view(auto_update) == "0");

    // SDL's log output reaches the engine's log front end.
    Lines().clear();
    SetLogSink(&Capture);
    SDL_Log("hello from SDL %d", 42);
    SetLogSink(nullptr);
    REQUIRE(Lines().size() == 1);
    CHECK(Lines()[0].find("hello from SDL 42") != std::string::npos);

    // Moving keeps one owner.
    SdlContext moved = std::move(*sdl);
    REQUIRE(moved.Subsystems() != 0);
    REQUIRE(sdl->Subsystems() == 0);  // NOLINT(bugprone-use-after-move)
  }
  REQUIRE(SDL_WasInit(SDL_INIT_VIDEO) == 0);
  SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
  SDL_Quit();
}

TEST_CASE("An unknown video driver is an error, not a crash", "[sdl]") {
  const testing::MainThreadScope main;
  SdlInitConfig config;
  config.subsystems = SDL_INIT_VIDEO;
  config.video_driver = "no-such-driver";
  auto sdl = SdlContext::Init(config);
  REQUIRE_FALSE(sdl);
  CHECK(sdl.error().message.find("SDL_InitSubSystem") != std::string::npos);
  REQUIRE(SDL_WasInit(SDL_INIT_VIDEO) == 0);
  SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
  SDL_Quit();
}

}  // namespace
}  // namespace se
