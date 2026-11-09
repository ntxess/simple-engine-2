// Copyright 2026 The simple-engine-2 Authors.
//
// Runs SDL's main-callback driver (SDL_EnterAppMainCallbacks, the loop an
// SDL_MAIN_USE_CALLBACKS executable runs) inside a test, with the four
// callbacks given as functions. Main thread only; one loop at a time. SDL
// calls SDL_Quit when the loop ends.

#ifndef TESTS_PLATFORM_SUPPORT_CALLBACK_LOOP_H_
#define TESTS_PLATFORM_SUPPORT_CALLBACK_LOOP_H_

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

#include <functional>
#include <string>
#include <vector>

namespace se::testing {

struct CallbackLoop {
  std::function<SDL_AppResult(void** appstate, int argc, char** argv)> init;
  std::function<SDL_AppResult(void* appstate)> iterate;
  std::function<SDL_AppResult(void* appstate, SDL_Event* event)> event;
  std::function<void(void* appstate, SDL_AppResult result)> quit;
};

// Runs |loop| with |args| as argv (argv[0] is added). Returns what
// SDL_EnterAppMainCallbacks returns: 0 after SUCCESS, 1 after FAILURE.
int RunCallbackLoop(CallbackLoop loop, const std::vector<std::string>& args);

// Seconds since SDL's tick origin.
double NowSec();

}  // namespace se::testing

#endif  // TESTS_PLATFORM_SUPPORT_CALLBACK_LOOP_H_
