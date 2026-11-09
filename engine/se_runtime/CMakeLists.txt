# se_runtime: the runtime on SDL's main callbacks (docs/milestone-specs.md,
# M5). So far its pure parts: the lifecycle and two-phase quit state
# machines, the frame clock, BasicInputService and AppConfig. Not
# deterministic.
#
# The spec's dependency list also names se_os, se_log and se_crash; the
# integration step adds them.

se_add_module(se_runtime KIND STATIC
  SOURCES
    src/app_config.cc
    src/basic_input.cc
    src/frame_clock.cc
    src/lifecycle.cc
    src/quit.cc
  PUBLIC_DEPS
    se_core
    se_sim
    se_platform
  FORBID
    se_imgui miniaudio sentry lz4_static)

# Test hooks (--fail-init-step, --inject-throw, --inject-null-acquire) in
# every build but Release.
target_compile_definitions(se_runtime PUBLIC
  $<IF:$<CONFIG:Release>,SE_RUNTIME_TEST_HOOKS=0,SE_RUNTIME_TEST_HOOKS=1>)
