// Copyright 2026 The simple-engine-2 Authors.

#include "platform/support/callback_loop.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
// Declarations only: Catch2 provides main().
#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_timer.h>

#include <string>
#include <utility>
#include <vector>

namespace se::testing {
namespace {

CallbackLoop& Current() {
  static auto* const kLoop = new CallbackLoop();
  return *kLoop;
}

SDL_AppResult SDLCALL Init(void** appstate, int argc, char** argv) {
  return Current().init ? Current().init(appstate, argc, argv)
                        : SDL_APP_CONTINUE;
}

SDL_AppResult SDLCALL Iterate(void* appstate) {
  return Current().iterate ? Current().iterate(appstate) : SDL_APP_SUCCESS;
}

SDL_AppResult SDLCALL Event(void* appstate, SDL_Event* event) {
  return Current().event ? Current().event(appstate, event) : SDL_APP_CONTINUE;
}

void SDLCALL Quit(void* appstate, SDL_AppResult result) {
  if (Current().quit) Current().quit(appstate, result);
}

}  // namespace

int RunCallbackLoop(CallbackLoop loop, const std::vector<std::string>& args) {
  Current() = std::move(loop);
  std::vector<std::string> storage;
  storage.emplace_back("se_platform_tests");
  storage.insert(storage.end(), args.begin(), args.end());
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& arg : storage) argv.push_back(arg.data());
  argv.push_back(nullptr);
  SDL_SetMainReady();
  const int result =
      SDL_EnterAppMainCallbacks(static_cast<int>(storage.size()), argv.data(),
                                &Init, &Iterate, &Event, &Quit);
  Current() = CallbackLoop();
  return result;
}

double NowSec() { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; }

}  // namespace se::testing
