# se_runtime: the App on SDL's main callbacks (docs/milestone-specs.md, M5):
# the Booting steps, the frame, the lifecycle and two-phase quit state
# machines, BasicInputService and the noexcept callbacks (sdl_app.cc, a
# fault boundary). Not deterministic. The M4 parts (the main thread's
# RoleScope, crash capture and its dump streams, the log worker, the
# watchdog) plug in through include/se_runtime/integration.h.

se_add_module(se_runtime KIND STATIC
  SOURCES
    src/app.cc
    src/app_config.cc
    src/basic_input.cc
    src/frame_clock.cc
    src/integration.cc
    src/lifecycle.cc
    src/quit.cc
    src/sdl_app.cc
  PUBLIC_DEPS
    se_core
    se_sim
    se_os
    se_log
    se_crash
    se_platform
    se_render
  FORBID
    se_imgui miniaudio sentry lz4_static)
# Internal headers are included as "src/<name>.h".
target_include_directories(se_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Test hooks (--fail-init-step, --inject-throw, --inject-null-acquire) in
# every build but Release.
target_compile_definitions(se_runtime PUBLIC
  $<IF:$<CONFIG:Release>,SE_RUNTIME_TEST_HOOKS=0,SE_RUNTIME_TEST_HOOKS=1>)
