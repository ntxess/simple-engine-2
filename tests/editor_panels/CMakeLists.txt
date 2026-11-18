# The editor panels' tests (docs/milestone-specs.md M10b "Tests"); test names
# start with "editor_panels.", labels unit and editor. Headless: no window,
# no GPU, no OS input.
#
#   se_editor_panel_tests   Catch2: the panels' pure parts (the waypoint
#                           canvas's controls and rules, the profiler's axis
#                           rule, picking by broadphase, exact bounds and
#                           brute force, the stress plan, the viewport's
#                           letterbox math, the gizmo, the overlays), and
#                           every panel drawn in a headless ImGui context
#                           (se_imgui's HeadlessFrames) driven by synthetic
#                           ImGui IO events, against a World built by the
#                           test with its gate closed and a recording
#                           EditorCommands or PlayEdits

se_add_module(se_editor_panel_tests KIND EXECUTABLE
  SOURCES
    gizmo_test.cc
    inspector_panel_test.cc
    overlays_test.cc
    panels_test.cc
    picking_test.cc
    prefab_panels_test.cc
    profiler_series_test.cc
    stress_generator_test.cc
    viewport_math_test.cc
    waypoint_canvas_test.cc
    support/panel_harness.cc
    ../support/no_dialogs_listener.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    game_rampage
    se_core
    se_editor
    se_editor_panels
    se_imgui
    se_render_core
    se_sim
    Catch2::Catch2WithMain
  TEST)
target_include_directories(se_editor_panel_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/tests")
# The prefab tests open the repository's sandbox and prefabs (read only).
target_compile_definitions(se_editor_panel_tests PRIVATE
  SE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

catch_discover_tests(se_editor_panel_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "editor_panels."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS "unit;editor"
    ENVIRONMENT SE_NO_MESSAGE_BOX=1)
