// Copyright 2026 The simple-engine-2 Authors.
//
// SDL_GPU performs no thread checks, and the swapchain belongs to the
// window's thread, so every se_render function that calls SDL_GPU asserts
// both the engine's main thread (registered at boot, ADR-009; AssertMainThread
// also tells the thread-safety analysis that g_main is held) and SDL's
// (SDL_IsMainThread: the thread that initialised video and events).

#ifndef ENGINE_SE_RENDER_SRC_GPU_ASSERT_H_
#define ENGINE_SE_RENDER_SRC_GPU_ASSERT_H_

#include <SDL3/SDL_init.h>

#include "se_core/assert.h"
#include "se_core/thread_role.h"

#define SE_ASSERT_GPU_THREAD()                   \
  do {                                           \
    ::se::AssertMainThread();                    \
    SE_ASSERT_MSG(SDL_IsMainThread(),            \
                  "SDL_GPU call off SDL's main " \
                  "thread");                     \
  } while (false)

#endif  // ENGINE_SE_RENDER_SRC_GPU_ASSERT_H_
