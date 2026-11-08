# game_rampage tests (docs/milestone-specs.md, M3b "Tests"); label rampage.
#
#   se_rampage_tests      Catch2: serialization

# game_rampage's link-graph contract: never SDL, ImGui, audio, crash or LZ4
# libraries (checked against every module's closure by se_check_link_graph).
get_target_property(_se_rampage_forbid game_rampage SE_FORBID)
foreach(_se_target SDL3::SDL3-static se_imgui miniaudio sentry lz4_static)
  if(NOT _se_target IN_LIST _se_rampage_forbid)
    message(FATAL_ERROR "game_rampage must FORBID ${_se_target}")
  endif()
endforeach()

se_add_module(se_rampage_tests KIND EXECUTABLE
  SOURCES
    serialization_test.cc
  PRIVATE_DEPS
    game_rampage
    se_core
    se_sim
    se_test_support
    Catch2::Catch2WithMain
  DETERMINISTIC
  TEST)

catch_discover_tests(se_rampage_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "rampage."
  ADD_TAGS_AS_LABELS
  PROPERTIES LABELS rampage)
