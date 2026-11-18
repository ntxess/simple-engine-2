# se_platform: SDL3 on the main thread (docs/milestone-specs.md, M5): SDL's
# initialisation, the event filter and Inbox, WakeMain, the window and its
# Win32 subclass, and se::fs on SDL's filesystem API. Not deterministic; may
# include <windows.h>. SDL3 is the pinned static library, unpatched until M6.
# M6b adds the display outputs (display_output.h) and the vblank sampler, an
# se_os thread (role kVblankSampler) waiting on the window's DXGI output.
# M8 adds the input sampler (role kInputSampler: SDL_UpdateGamepads at
# 1 kHz, parked while the app is Suspended or Idle) and the open gamepads.
# M9 adds the stepwise file streams (file_stream.h: FILE_SHARE_DELETE reads,
# TryReplace's sharing violations) and the fault and delay injector
# (fs_faults.h, test builds) for the background lane.
# M12 adds the in-driver flag (driver_wait.h) that se_render and the
# FramePacer raise around SDL_GPU and pacer waits, for the watchdog's label.

se_use_deps(sdl3)

se_add_module(se_platform KIND STATIC
  SOURCES
    src/display_output.cc
    src/driver_wait.cc
    src/fatal.cc
    src/file_stream.cc
    src/fs.cc
    src/fs_faults.cc
    src/gamepads.cc
    src/inbox.cc
    src/input_sampler.cc
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

# The se::fs fault and delay injector (fs_faults.h) in every build but
# Release (M9).
target_compile_definitions(se_platform PUBLIC
  $<IF:$<CONFIG:Release>,SE_FS_TEST_HOOKS=0,SE_FS_TEST_HOOKS=1>)

if(WIN32)
  # SetWindowSubclass and RemoveWindowSubclass; DXGI for the vblank sampler
  # and the display outputs (the only module that links it).
  target_link_libraries(se_platform PRIVATE comctl32 dxgi)
endif()
