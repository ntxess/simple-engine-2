// Copyright 2026 The simple-engine-2 Authors.
//
// Include in exactly one file of an executable, its main.cc, and follow it
// with SE_DEFINE_SDL_APP(factory) (se_runtime/sdl_app.h). It defines
// SDL_MAIN_USE_CALLBACKS and includes SDL_main.h, whose header-only part
// provides main() and enters SDL's main-callback loop.

#ifndef SE_RUNTIME_SDL_MAIN_H_
#define SE_RUNTIME_SDL_MAIN_H_

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#include "se_runtime/sdl_app.h"

#endif  // SE_RUNTIME_SDL_MAIN_H_
