# The editor panels' tests (docs/milestone-specs.md M10b "Tests"); labels
# unit and editor. Headless: no window, no GPU, no OS input.
#
#   se_editor_panel_tests   Catch2: the waypoint canvas's controls and
#                           path-editing rules, the profiler's axis rule,
#                           picking (broadphase, exact, brute force), the
#                           stress generator's plan, the viewport's
#                           letterbox math and the entity overlays

set(_se_panels_out "${CMAKE_BINARY_DIR}/editor_panels_out")
file(MAKE_DIRECTORY "${_se_panels_out}")

se_add_module(se_editor_panel_tests KIND EXECUTABLE
  SOURCES
    overlays_test.cc
    picking_test.cc
    profiler_series_test.cc
    stress_generator_test.cc
    viewport_math_test.cc
    waypoint_canvas_test.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    game_rampage
    se_core
    se_editor_panels
    se_render_core
    se_sim
    se_test_support
    Catch2::Catch2WithMain
  TEST)
target_include_directories(se_editor_panel_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/tests")
target_compile_definitions(se_editor_panel_tests PRIVATE
  SE_TEST_OUT_DIR="${_se_panels_out}")

catch_discover_tests(se_editor_panel_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "editor_panels."
  ADD_TAGS_AS_LABELS
  PROPERTIES LABELS "unit;editor")
