// Copyright 2026 The simple-engine-2 Authors.
//
// The four SDL main callbacks of an engine executable (docs/milestone-specs.md
// M5, sdl_app.h; docs/design/mt-design.md "Platform"). An executable's
// main.cc includes "se_runtime/sdl_main.h" (which defines
// SDL_MAIN_USE_CALLBACKS and includes SDL_main.h) and then writes
//
//   SE_DEFINE_SDL_APP(&MakeMyGame)
//
// where MakeMyGame is a GameFactory. The callbacks below are noexcept and
// catch everything (sdl_app.cc is a fault boundary, ADR-006), so no
// exception unwinds through SDL's C frames:
//
//   SdlAppInit    registers the main thread, parses the command line and
//                 runs App::Create; *appstate is set only after a complete
//                 boot. A failed boot shows a message box (unless
//                 SE_NO_MESSAGE_BOX=1) and returns SDL_APP_FAILURE.
//   SdlAppIterate, SdlAppEvent
//                 App::Iterate / App::HandleEvent. A fault requests a dump
//                 (integration hook), shows a message box and returns
//                 SDL_APP_FAILURE: SDL then runs the quit path.
//   SdlAppQuit    App::Shutdown; with a NULL appstate (failed boot) only
//                 logging shuts down. Release builds then end the process
//                 after the final flush (unless --return-on-quit); other
//                 builds return and SDL calls SDL_Quit.

#ifndef SE_RUNTIME_SDL_APP_H_
#define SE_RUNTIME_SDL_APP_H_

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

#include "se_runtime/app.h"

namespace se {

SDL_AppResult SdlAppInit(void** appstate, int argc, char** argv,
                         GameFactory factory) noexcept;
SDL_AppResult SdlAppIterate(void* appstate) noexcept;
SDL_AppResult SdlAppEvent(void* appstate, SDL_Event* event) noexcept;
void SdlAppQuit(void* appstate, SDL_AppResult result) noexcept;

// Whether SdlAppQuit ends the process after shutdown. SdlAppInit sets it
// from the command line; tests that call SdlAppQuit directly set it off.
void SetTerminateOnQuit(bool terminate) noexcept;

}  // namespace se

// Defines SDL_AppInit, SDL_AppIterate, SDL_AppEvent and SDL_AppQuit for an
// executable built with SDL_MAIN_USE_CALLBACKS (see sdl_main.h).
#define SE_DEFINE_SDL_APP(factory)                                             \
  SDL_AppResult SDLCALL SDL_AppInit(void** appstate, int argc, char* argv[]) { \
    return ::se::SdlAppInit(appstate, argc, argv, (factory));                  \
  }                                                                            \
  SDL_AppResult SDLCALL SDL_AppIterate(void* appstate) {                       \
    return ::se::SdlAppIterate(appstate);                                      \
  }                                                                            \
  SDL_AppResult SDLCALL SDL_AppEvent(void* appstate, SDL_Event* event) {       \
    return ::se::SdlAppEvent(appstate, event);                                 \
  }                                                                            \
  void SDLCALL SDL_AppQuit(void* appstate, SDL_AppResult result) {             \
    ::se::SdlAppQuit(appstate, result);                                        \
  }

#endif  // SE_RUNTIME_SDL_APP_H_
