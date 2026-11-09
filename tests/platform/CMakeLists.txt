# Platform tests (docs/milestone-specs.md, M5 "Tests"; test names start
# with "platform."). PROPERTIES takes one value per property: catch's
# discovery would split a list value into further property names.
#
#   se_platform_tests   Catch2. Tests tagged [desktop] open real windows on
#                       the interactive desktop (label desktop: excluded from
#                       the commit-gate presets, run by scripts/ci.ps1
#                       -Desktop); the others are label unit and run
#                       everywhere, including the WSL TSan and ASan builds.
#
# Desktop tests never move the mouse or send real keystrokes, open their
# windows without activating them, and run with SE_NO_MESSAGE_BOX=1.
#
# The platform defines the real WakeMain(), so these binaries do not link
# se_test_support (which carries the null one).

set(_se_platform_out "${CMAKE_BINARY_DIR}/platform_out")
file(MAKE_DIRECTORY "${_se_platform_out}")

se_add_module(se_platform_test_support KIND STATIC
  SOURCES
    support/callback_loop.cc
    support/test_app.cc
  PUBLIC_DEPS
    se_platform
    se_runtime
    se_sim
  TEST)
target_include_directories(se_platform_test_support PUBLIC
  "${CMAKE_SOURCE_DIR}/tests")
target_compile_definitions(se_platform_test_support PRIVATE
  SE_TEST_OUT_DIR="${_se_platform_out}")

se_add_module(se_platform_tests KIND EXECUTABLE
  SOURCES
    app_desktop_test.cc
    callbacks_test.cc
    fatal_test.cc
    fs_test.cc
    gpu_test.cc
    inbox_test.cc
    lifecycle_test.cc
    quit_test.cc
    runtime_test.cc
    sdl_context_test.cc
    wake_test.cc
    window_test.cc
    ../support/no_dialogs_listener.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    se_core
    se_platform
    se_platform_test_support
    se_render
    se_runtime
    se_sim
    Catch2::Catch2WithMain
  TEST)
if(WIN32)
  target_link_libraries(se_platform_tests PRIVATE comctl32)
endif()
target_compile_definitions(se_platform_tests PRIVATE
  SE_TEST_OUT_DIR="${_se_platform_out}")

catch_discover_tests(se_platform_tests
  TEST_SPEC "~[desktop]"
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "platform."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS unit
    ENVIRONMENT SE_NO_MESSAGE_BOX=1)
catch_discover_tests(se_platform_tests
  TEST_SPEC "[desktop]"
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "platform."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS desktop
    ENVIRONMENT SE_NO_MESSAGE_BOX=1
    TIMEOUT 60)

# A failed SDL assertion prints and aborts, never opening SDL's dialog
# (ADR-070): with a dialog the test would hang until its timeout. CTest
# counts a signal as a failure whatever the output, so run_app.py checks
# the abort and the printed line.
add_test(NAME platform.sdl_assertion_aborts
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/run_app.py"
          --expect-abort --expect "SDL assertion failed: 'holds'"
          --timeout 30 -- $<TARGET_FILE:se_platform_tests> "[.sdl_assertion]")
set_tests_properties(platform.sdl_assertion_aborts PROPERTIES
  LABELS unit TIMEOUT 60)

# The rampage executable (label desktop): it runs the sandbox for 300
# presented frames and quits through the two-phase quit, and a failure at
# every boot step exits with SDL_APP_FAILURE, no app state and no engine
# thread left (scripts/ci.ps1 -Desktop runs these in msvc-dev and, for ASan,
# msvc-asan). run_app.py checks the exit status as well as the output; the
# patterns avoid backslashes, which the test command line does not keep.
if(TARGET rampage)
  set(_se_run_app "${CMAKE_CURRENT_SOURCE_DIR}/run_app.py")
  set(_se_rampage_pref "${_se_platform_out}/rampage_pref")
  add_test(NAME rampage.frames_300
    COMMAND Python3::Interpreter "${_se_run_app}" --expect-exit 0
            --expect "booted 'rampage'"
            --expect "shutdown [(]success[)]: [0-9]+ iterates, 300 presented frames, ran [1-9][0-9][0-9]+ ticks"
            --expect "log file .*rampage_pref/logs/rampage[.]log"
            --reject "SE_ASSERT failed|ERROR|E t=[0-9]+ | [EF] [a-z-]+ t=[0-9]+ "
            -- $<TARGET_FILE:rampage> --frames 300 --no-activate
            --pref-dir "${_se_rampage_pref}")
  set_tests_properties(rampage.frames_300 PROPERTIES
    LABELS "desktop;gpu;rampage" TIMEOUT 120)
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    foreach(_se_step RANGE 1 10)
      add_test(NAME rampage.fail_init_step.${_se_step}
        COMMAND Python3::Interpreter "${_se_run_app}" --expect-exit 1
                --expect "injected failure at boot step ${_se_step} "
                --expect "SDL_AppQuit without an app [(]boot failed[)]: 0 engine threads running"
                --reject "booted 'rampage'|SE_ASSERT failed"
                -- $<TARGET_FILE:rampage> --fail-init-step ${_se_step}
                --no-activate --pref-dir "${_se_rampage_pref}")
      set_tests_properties(rampage.fail_init_step.${_se_step} PROPERTIES
        LABELS "desktop;rampage" TIMEOUT 60)
    endforeach()
  endif()
endif()
