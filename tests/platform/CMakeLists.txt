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
    app_input_test.cc
    assets_app_test.cc
    callbacks_test.cc
    fatal_test.cc
    fs_test.cc
    gpu_test.cc
    gamepad_desktop_test.cc
    helper_threads_test.cc
    inbox_test.cc
    input_sampler_test.cc
    input_test.cc
    lifecycle_test.cc
    sound_assets_desktop_test.cc
    quit_test.cc
    runtime_test.cc
    sdl_context_test.cc
    wake_test.cc
    window_test.cc
    ../support/no_dialogs_listener.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    game_rampage
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
  SE_TEST_OUT_DIR="${_se_platform_out}"
  SE_REPO_ROOT="${CMAKE_SOURCE_DIR}")

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

# M13a (test names start with "runtime."): the App with the content test
# module (tests/support/content_test_module.h) headless: the start state
# and the content gate, a changed text and kAssetSwap, focus loss through
# the SessionLog, the empty stack and a state's failed text. Label unit.
se_add_module(se_runtime_content_tests KIND EXECUTABLE
  SOURCES
    runtime_content_test.cc
    ../support/no_dialogs_listener.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    se_core
    se_platform
    se_platform_test_support
    se_content_test_support
    se_runtime
    se_sim
    Catch2::Catch2WithMain
  TEST)
target_compile_definitions(se_runtime_content_tests PRIVATE
  SE_TEST_OUT_DIR="${_se_platform_out}")
catch_discover_tests(se_runtime_content_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "runtime."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS unit
    ENVIRONMENT SE_NO_MESSAGE_BOX=1
    TIMEOUT 120)

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
            --expect "audio: 2 sounds in config/sound[.]json, decoded on the background lane"
            --expect "sound 'hit' resident: [0-9]+ frames"
            --expect "sound 'death' resident: [0-9]+ frames"
            --expect "state 'sandbox': its 5 assets are resident after"
            --expect "shutdown order: input-sampler, event-filter [(]4[)], worlds [(]6[)], audio [(]7[)], bg-lane [(]8[)], vblank-sampler [(]9[)], asset-scopes [(]10[)]"
            --reject "SE_ASSERT failed|ERROR|E t=[0-9]+ | [EF] [a-z-]+ t=[0-9]+ "
            -- $<TARGET_FILE:rampage> --frames 300 --no-activate
            --pref-dir "${_se_rampage_pref}")
  set_tests_properties(rampage.frames_300 PROPERTIES
    LABELS "desktop;gpu;rampage" TIMEOUT 120)
  # The integration (ADR-089): rampage quits with a Durable save in flight
  # through the lane on the throttled-disk injector, and the quit waits for
  # the rename (durable_quit_test.py). A test hook, so not in Release.
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    add_test(NAME platform.rampage_durable_quit
      COMMAND Python3::Interpreter
              "${CMAKE_CURRENT_SOURCE_DIR}/durable_quit_test.py"
              --rampage $<TARGET_FILE:rampage>
              --work "${_se_platform_out}/rampage_durable_quit")
    set_tests_properties(platform.rampage_durable_quit PROPERTIES
      LABELS "desktop;rampage" TIMEOUT 120)
  endif()
  # M8: key events pushed with explicit timestamps 8 ms apart land on the
  # two ticks of a 60 Hz Locked frame, the recorded session replays
  # headless through se_replay with identical per-tick hashes, and the
  # edges keep their sources (input_replay_test.py; the --push-key hook is
  # a test hook, so not in Release). The windowed run is a desktop test; the
  # headless one runs everywhere.
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    if(WIN32)
      set(_se_key_source raw-keyboard)
    else()
      set(_se_key_source win-message)
    endif()
    foreach(_se_mode windowed headless)
      set(_se_mode_args)
      set(_se_mode_labels "desktop;rampage")
      if(_se_mode STREQUAL "headless")
        set(_se_mode_args --headless)
        set(_se_mode_labels "unit;rampage")
      endif()
      add_test(NAME input.pushed_keys_replay_${_se_mode}
        COMMAND Python3::Interpreter
                "${CMAKE_CURRENT_SOURCE_DIR}/input_replay_test.py"
                --rampage $<TARGET_FILE:rampage>
                --se-replay $<TARGET_FILE:se_replay>
                --scene "${CMAKE_SOURCE_DIR}/assets/scenes/sandbox.scene.json"
                --out "${_se_platform_out}/input_replay_${_se_mode}"
                --source ${_se_key_source} ${_se_mode_args})
      set_tests_properties(input.pushed_keys_replay_${_se_mode} PROPERTIES
        LABELS "${_se_mode_labels}" TIMEOUT 120)
    endforeach()
  endif()
  # M13a: the runtime's quit durability with rampage headless and its
  # --record replay as the Durable save (quit_durability_test.py; the
  # --fs-faults injector is a test hook, so not in Release), and the
  # shutdown stress: 100 headless runs quitting by --frames, a pushed
  # SDL_EVENT_QUIT and SDL_EVENT_TERMINATING (label slow).
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    foreach(_se_scenario replay_throttled deadline_after_drain
                         failed_durable_save)
      add_test(NAME platform.quit_durability.${_se_scenario}
        COMMAND Python3::Interpreter
                "${CMAKE_CURRENT_SOURCE_DIR}/quit_durability_test.py"
                --rampage $<TARGET_FILE:rampage>
                --se-replay $<TARGET_FILE:se_replay>
                --work "${_se_platform_out}/quit_durability_${_se_scenario}"
                --scenario ${_se_scenario})
      set_tests_properties(platform.quit_durability.${_se_scenario}
        PROPERTIES LABELS "unit;rampage" TIMEOUT 120)
    endforeach()
    add_test(NAME platform.shutdown_stress
      COMMAND Python3::Interpreter
              "${CMAKE_CURRENT_SOURCE_DIR}/shutdown_stress_test.py"
              --rampage $<TARGET_FILE:rampage>
              --work "${_se_platform_out}/shutdown_stress")
    set_tests_properties(platform.shutdown_stress PROPERTIES
      LABELS "slow;rampage" TIMEOUT 3600)
  endif()
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
