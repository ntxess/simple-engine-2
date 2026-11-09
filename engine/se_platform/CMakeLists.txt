# se_platform: SDL3 on the main thread (docs/milestone-specs.md, M5): SDL's
# initialisation, the event filter and Inbox, WakeMain, the window and its
# Win32 subclass, and se::fs on SDL's filesystem API. Not deterministic; may
# include <windows.h>. SDL3 is the pinned static library, unpatched until M6.
# M6b adds the display outputs (display_output.h) and the vblank sampler, an
# se_os thread (role kVblankSampler) waiting on the window's DXGI output.

se_use_deps(sdl3)

se_add_module(se_platform KIND STATIC
  SOURCES
    src/display_output.cc
    src/fatal.cc
    src/fs.cc
    src/inbox.cc
    src/sdl_context.cc
    src/vblank_sampler.cc
    src/wake.cc
    src/window.cc
  PUBLIC_DEPS
    se_core
    se_os
    SDL3::SDL3-static
  FORBID
    se_imgui miniaudio sentry lz4_static)

if(WIN32)
  # SetWindowSubclass and RemoveWindowSubclass; DXGI for the vblank sampler
  # and the display outputs (the only module that links it).
  target_link_libraries(se_platform PRIVATE comctl32 dxgi)
endif()
