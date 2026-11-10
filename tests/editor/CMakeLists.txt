# Editor tests (docs/milestone-specs.md M10a "Tests (M10a)"; test names
# start with "editor."). Headless everywhere: ImGui frames without a window
# (se_imgui's HeadlessFrames), documents and commands on rampage's
# registry, Play on the editor's sessions, the dialog mailbox and the lane
# (label unit; TSan in WSL where threads are involved). The desktop tests
# run the simple_editor executable through scripts (label desktop,
# tests/editor/*.py).

se_add_module(se_editor_tests KIND EXECUTABLE
  SOURCES
    commands_test.cc
    imgui_context_test.cc
    ../support/no_dialogs_listener.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    se_editor
    se_imgui
    game_rampage
    se_core
    se_sim
    Catch2::Catch2WithMain
  TEST)
target_include_directories(se_editor_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
target_compile_definitions(se_editor_tests PRIVATE
  SE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

catch_discover_tests(se_editor_tests
  TEST_SPEC "~[desktop]"
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "editor."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS unit
    ENVIRONMENT SE_NO_MESSAGE_BOX=1)
