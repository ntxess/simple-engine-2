# Pacing tests (docs/milestone-specs.md M6b; test names start with
# "pacing."):
#
#   se_pacing_tests   Catch2. PacerPolicy, the FramePacer shell on a fake
#                     clock and present port (tests/support/
#                     fake_present_port.h), and the vblank sampler's loop on a
#                     fake vblank source: label unit, everywhere, TSan
#                     included. Tests tagged [desktop] open real windows
#                     (the sampler following its window across monitors, the
#                     display-rate investigation, the App's pacing modes):
#                     label desktop, run by scripts/ci.ps1 -Desktop, Windows
#                     only; they never move the cursor or send input and open
#                     their windows without activating them.
#
# Like tests/platform, the binary links the real WakeMain() through
# se_runtime and so not se_test_support.

set(_se_pacing_out "${CMAKE_BINARY_DIR}/pacing_out")
file(MAKE_DIRECTORY "${_se_pacing_out}")

se_add_module(se_pacing_tests KIND EXECUTABLE
  SOURCES
    frame_pacer_test.cc
    pacer_policy_test.cc
    pacing_desktop_test.cc
    vblank_sampler_test.cc
    ../support/fake_present_port.cc
    ../support/no_dialogs_listener.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    game_rampage
    se_core
    se_os
    se_platform
    se_platform_test_support
    se_render
    se_runtime
    se_sim
    Catch2::Catch2WithMain
  TEST)
target_include_directories(se_pacing_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/tests")
target_compile_definitions(se_pacing_tests PRIVATE
  SE_TEST_OUT_DIR="${_se_pacing_out}"
  SE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

catch_discover_tests(se_pacing_tests
  TEST_SPEC "~[desktop]"
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "pacing."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS unit
    ENVIRONMENT SE_NO_MESSAGE_BOX=1)
catch_discover_tests(se_pacing_tests
  TEST_SPEC "[desktop]"
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "pacing."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS desktop
    ENVIRONMENT SE_NO_MESSAGE_BOX=1
    TIMEOUT 120)

# tools/presentmon/analyze.py on a synthetic PresentMon CSV (any platform).
add_test(NAME presentmon.analyze
  COMMAND Python3::Interpreter
          "${CMAKE_SOURCE_DIR}/tools/presentmon/analyze_test.py")
set_tests_properties(presentmon.analyze PROPERTIES LABELS tools)
