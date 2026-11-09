# se_platform: SDL3 on the main thread (docs/milestone-specs.md, M5): SDL's
# initialisation, the event filter and Inbox, WakeMain, the window and its
# Win32 subclass, and se::fs on SDL's filesystem API. Not deterministic; may
# include <windows.h>. SDL3 is the pinned static library, unpatched until M6.

se_use_deps(sdl3)

se_add_module(se_platform KIND STATIC
  SOURCES
    src/fatal.cc
    src/fs.cc
    src/inbox.cc
    src/sdl_context.cc
    src/wake.cc
    src/window.cc
  PUBLIC_DEPS
    se_core
    SDL3::SDL3-static
  FORBID
    se_imgui miniaudio sentry lz4_static)

if(WIN32)
  # SetWindowSubclass and RemoveWindowSubclass.
  target_link_libraries(se_platform PRIVATE comctl32)
endif()
