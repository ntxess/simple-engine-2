# Pacing tests (docs/milestone-specs.md M6b; test names start with
# "pacing."):
#
#   se_pacing_tests   Catch2. PacerPolicy (pure: the mode, the period, the
#                     paced wake, misses, HalfRate, latency): label unit,
#                     everywhere, TSan included.
#
# Like tests/platform, the binary links the real WakeMain() through
# se_runtime and so not se_test_support.

se_add_module(se_pacing_tests KIND EXECUTABLE
  SOURCES
    pacer_policy_test.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    se_core
    se_os
    se_platform
    se_runtime
    Catch2::Catch2WithMain
  TEST)
target_include_directories(se_pacing_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/tests")

catch_discover_tests(se_pacing_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "pacing."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS unit)
