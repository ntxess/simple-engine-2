# se_runtime: the App on SDL's main callbacks (docs/milestone-specs.md, M5):
# the Booting steps, the frame, the lifecycle and two-phase quit state
# machines, BasicInputService and the noexcept callbacks (sdl_app.cc, a
# fault boundary). Not deterministic. The M4 parts (the main thread's
# RoleScope, crash capture and its dump streams, the log worker, the
# watchdog) plug in through include/se_runtime/integration.h. M6b adds the
# FramePacer (frame_pacer.h, over the pure PacerPolicy) and the PresentPort
# it presents through (present_port.h); the M6/M7 integration the adapter
# over se::present and Renderer2D (render_present_port.h) and device-loss
# recovery. M9 adds the background lane and the AssetRegistry
# (asset_runtime.h: a scope per GameState, the hold until it is resident,
# GpuResources behind the registry's upload port) and the lane's
# QuitProvider (lane_quit.h).

se_add_module(se_runtime KIND STATIC
  SOURCES
    src/app.cc
    src/app_config.cc
    src/asset_runtime.cc
    src/basic_input.cc
    src/frame_clock.cc
    src/frame_pacer.cc
    src/integration.cc
    src/lane_quit.cc
    src/lifecycle.cc
    src/pacer_policy.cc
    src/quit.cc
    src/render_present_port.cc
    src/sdl_app.cc
  PUBLIC_DEPS
    se_core
    se_sim
    se_os
    se_assets
    se_log
    se_crash
    se_platform
    se_render
  # lz4_static is reachable through se_assets (M9), which alone includes
  # it (se_assets' BANNED_INCLUDES do not list it; every other module's do).
  FORBID
    se_imgui miniaudio sentry)
# Internal headers are included as "src/<name>.h".
target_include_directories(se_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Test hooks (--fail-init-step, --inject-throw, --inject-null-acquire,
# --inject-late-frame, --inject-device-lost, M7's) in every build but
# Release.
target_compile_definitions(se_runtime PUBLIC
  $<IF:$<CONFIG:Release>,SE_RUNTIME_TEST_HOOKS=0,SE_RUNTIME_TEST_HOOKS=1>)
