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
  PUBLIC_DEPS
    se_platform
  TEST)
target_include_directories(se_platform_test_support PUBLIC
  "${CMAKE_SOURCE_DIR}/tests")

se_add_module(se_platform_tests KIND EXECUTABLE
  SOURCES
    fs_test.cc
    inbox_test.cc
    sdl_context_test.cc
    wake_test.cc
    window_test.cc
  PRIVATE_DEPS
    se_core
    se_platform
    se_platform_test_support
    Catch2::Catch2WithMain
  TEST)
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

