# game_rampage tests (docs/milestone-specs.md, M3b "Tests"); label rampage.
#
#   se_rampage_tests      Catch2: serialization, broadphase, effects, path,
#                         boundary, JSON, scenes and the legacy scenarios

# game_rampage's link-graph contract: never SDL, ImGui, audio, crash or LZ4
# libraries (checked against every module's closure by se_check_link_graph).
get_target_property(_se_rampage_forbid game_rampage SE_FORBID)
foreach(_se_target SDL3::SDL3-static se_imgui miniaudio sentry lz4_static)
  if(NOT _se_target IN_LIST _se_rampage_forbid)
    message(FATAL_ERROR "game_rampage must FORBID ${_se_target}")
  endif()
endforeach()

set(_se_rampage_out "${CMAKE_BINARY_DIR}/rampage_out")
file(MAKE_DIRECTORY "${_se_rampage_out}")

se_add_module(rampage_test_support KIND STATIC
  SOURCES
    support/rampage_session.cc
    support/seinput.cc
  PUBLIC_DEPS
    game_rampage
    se_core
    se_sim
  DETERMINISTIC
  TEST)
target_include_directories(rampage_test_support PUBLIC
  "${CMAKE_SOURCE_DIR}/tests")
target_compile_definitions(rampage_test_support PRIVATE
  SE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

se_add_module(se_rampage_tests KIND EXECUTABLE
  SOURCES
    boundary_test.cc
    broadphase_test.cc
    effects_test.cc
    json_test.cc
    legacy_scenario_test.cc
    path_test.cc
    scene_test.cc
    serialization_test.cc
  PRIVATE_DEPS
    game_rampage
    rampage_test_support
    se_core
    se_sim
    se_test_support
    Catch2::Catch2WithMain
  DETERMINISTIC
  TEST)

target_compile_definitions(se_rampage_tests PRIVATE
  SE_TEST_OUT_DIR="${_se_rampage_out}")

catch_discover_tests(se_rampage_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "rampage."
  ADD_TAGS_AS_LABELS
  PROPERTIES LABELS rampage)
