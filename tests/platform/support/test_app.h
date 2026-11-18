// Copyright 2026 The simple-engine-2 Authors.
//
// Runs the real App (se_runtime) inside a test, under SDL's main-callback
// driver, with the platform test module: one state ("play") holding one
// empty World, so ticks are cheap and countable. Every run passes
// --return-on-quit and a --pref-dir in the build tree; windowed runs add
// --no-activate, so no window takes focus.
//
// Every run's teardown (SDL_AppQuit) is measured, and one that takes
// kTeardownLimitSec or longer fails the run (M12: CI keeps teardown under
// 2 s): RunTestApp then returns kSlowTeardown, which no test expects.

#ifndef TESTS_PLATFORM_SUPPORT_TEST_APP_H_
#define TESTS_PLATFORM_SUPPORT_TEST_APP_H_

#include <string>
#include <vector>

#include "se_runtime/app.h"
#include "se_sim/game_module.h"

namespace se::testing {

// The platform test module.
[[nodiscard]] const GameModule& PlatformTestModule();

// A GameSetup for the test module; tests add hooks.
[[nodiscard]] GameSetup TestGameSetup();

// While set, the "play" state's OnExit throws std::runtime_error before it
// destroys its World (the shutdown fault tests).
void SetPlayExitThrows(bool throws);

// The longest teardown a run may take, and what RunTestApp returns after a
// longer one.
inline constexpr double kTeardownLimitSec = 2.0;
inline constexpr int kSlowTeardown = 99;

// Runs an App made from |setup| with |args| (plus the defaults above).
// Returns what SDL_EnterAppMainCallbacks returns: 0 after SUCCESS, 1 after
// FAILURE (a failed boot included), or kSlowTeardown.
int RunTestApp(GameSetup setup, std::vector<std::string> args);

// How long the last run's SDL_AppQuit took, in seconds.
[[nodiscard]] double LastTeardownSec();

}  // namespace se::testing

#endif  // TESTS_PLATFORM_SUPPORT_TEST_APP_H_
