// Copyright 2026 The simple-engine-2 Authors.
//
// Fault dialogs (ADR-070): with message boxes suppressed a failed SDL
// assertion prints and aborts instead of opening SDL's Abort/Retry/Ignore
// dialog, which would hang an unattended test until its timeout. The
// hidden case is run by CTest (platform.sdl_assertion_aborts) and must end
// with the report.

#include "se_platform/fatal.h"

#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_stdinc.h>

#include <catch2/catch_test_macros.hpp>

namespace se {
namespace {

bool DefaultAssertionHandler() {
  return SDL_GetAssertionHandler(nullptr) == SDL_GetDefaultAssertionHandler();
}

TEST_CASE("Test executables never show SDL's assertion dialog", "[fatal]") {
  // tests/support/no_dialogs_listener.cc ran before the first test case.
  CHECK_FALSE(DefaultAssertionHandler());
}

TEST_CASE("SE_NO_MESSAGE_BOX=1 replaces SDL's assertion dialog", "[fatal]") {
  SDL_SetAssertionHandler(nullptr, nullptr);
  REQUIRE(DefaultAssertionHandler());
  REQUIRE(SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SE_NO_MESSAGE_BOX",
                                     "1", true));
  REQUIRE(MessageBoxesSuppressed());
  ConfigureFaultDialogs();
  CHECK_FALSE(DefaultAssertionHandler());
  SuppressFaultDialogs();
}

TEST_CASE("A failed SDL assertion prints and aborts", "[.sdl_assertion]") {
  // Not a constant: the compiler would warn about the macro's loop.
  volatile bool holds = false;
  SDL_assert_release(holds);
  FAIL("SDL_assert_release returned");
}

}  // namespace
}  // namespace se
