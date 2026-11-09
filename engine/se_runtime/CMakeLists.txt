# se_runtime: the App on SDL's main callbacks (docs/milestone-specs.md, M5):
# the Booting steps, the frame, the lifecycle and two-phase quit state
# machines, BasicInputService and the noexcept callbacks (sdl_app.cc, a
# fault boundary). Not deterministic.
#
# The spec's dependency list also names se_os, se_log and se_crash; the
# integration step adds them, filling in the hooks of
# include/se_runtime/integration.h.

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
    se_platform
    se_render
  FORBID
    se_imgui miniaudio sentry lz4_static)

# Test hooks (--fail-init-step, --inject-throw, --inject-null-acquire) in
# every build but Release.
target_compile_definitions(se_runtime PUBLIC
  $<IF:$<CONFIG:Release>,SE_RUNTIME_TEST_HOOKS=0,SE_RUNTIME_TEST_HOOKS=1>)
