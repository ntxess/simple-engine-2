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
    dialog_mailbox_test.cc
    document_test.cc
    idle_test.cc
    imgui_context_test.cc
    play_session_test.cc
    recovery_test.cc
    script_test.cc
    ../support/no_dialogs_listener.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    se_editor
    se_imgui
    game_rampage
    rampage_test_support
    se_core
    se_sim
    Catch2::Catch2WithMain
  TEST)
target_include_directories(se_editor_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
# The tests write their files here.
set(_se_editor_out "${CMAKE_CURRENT_BINARY_DIR}/out")
file(MAKE_DIRECTORY "${_se_editor_out}")
target_compile_definitions(se_editor_tests PRIVATE
  SE_SOURCE_DIR="${CMAKE_SOURCE_DIR}" SE_TEST_OUT_DIR="${_se_editor_out}")

catch_discover_tests(se_editor_tests
  TEST_SPEC "~[desktop]"
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "editor."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS unit
    ENVIRONMENT SE_NO_MESSAGE_BOX=1)

# The script tests: simple_editor runs tests/editor/scripts/*.editor through
# run_editor.py. Headless (SDL's dummy video, a synthetic 120 Hz clock;
# label unit, everywhere) and in an unfocused window (label desktop).
# --editor-script needs the runtime's test hooks, which Release lacks.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" AND TARGET simple_editor)
  set(_se_run_editor "${CMAKE_CURRENT_SOURCE_DIR}/run_editor.py")
  set(_se_scripts "${CMAKE_CURRENT_SOURCE_DIR}/scripts")
  set(_se_sandbox "${CMAKE_SOURCE_DIR}/assets/scenes/sandbox.scene.json")
  # editor.script.NAME: script NAME headless, with run_editor.py's ARGN.
  function(_se_editor_script name script)
    add_test(NAME editor.script.${name}
      COMMAND Python3::Interpreter "${_se_run_editor}"
              --editor $<TARGET_FILE:simple_editor>
              --script "${_se_scripts}/${script}.editor" --headless ${ARGN})
    set_tests_properties(editor.script.${name} PROPERTIES
      LABELS unit TIMEOUT 180)
  endfunction()
  function(_se_editor_desktop name script)
    add_test(NAME editor.desktop.${name}
      COMMAND Python3::Interpreter "${_se_run_editor}"
              --editor $<TARGET_FILE:simple_editor>
              --script "${_se_scripts}/${script}.editor" ${ARGN})
    set_tests_properties(editor.desktop.${name} PROPERTIES
      LABELS desktop TIMEOUT 180)
  endfunction()

  _se_editor_script(edit_commands edit_commands --scene-copy "${_se_sandbox}")
  # The hotkeys as ImGui key events: Ctrl+Z, Ctrl+Y, Ctrl+S (saved through
  # the lane), F5, F10, Shift+F5.
  _se_editor_script(hotkeys hotkeys --scene-copy "${_se_sandbox}"
    --expect-scene-contains "321" --expect "play: step to tick")
  # The editor session replays headless to the same per-tick hashes.
  _se_editor_script(play_session_replays play_session
    --replay $<TARGET_FILE:se_replay>)
  # 'Play game': the module's own stack on the Play-start bytes.
  _se_editor_script(play_game play_game --expect "play game: started")
  _se_editor_script(tick_fault tick_fault --expect-fault-files
    --expect "Play.Faulted: tick fault")
  _se_editor_script(render_fault render_fault --expect-fault-files
    --expect "Play.Faulted: render fault")
  _se_editor_script(quit_save quit_save --scene-copy "${_se_sandbox}"
    --expect-scene-contains "321" --expect "quit ready after")
  _se_editor_script(quit_save_fails quit_save_fails
    --scene-copy "${_se_sandbox}" --expect-scene-unchanged
    --expect "editor: quit cancelled")
  _se_editor_script(dialog_quit dialog_quit
    --expect "1 answered during the quit")
  _se_editor_script(dialog_open dialog_open)
  _se_editor_script(autosave_recovers autosave --scene-copy "${_se_sandbox}"
    --then-script "${_se_scripts}/recover.editor"
    --expect "editor: recovered")
  _se_editor_script(terminate_writes_recovery terminate
    --scene-copy "${_se_sandbox}" --expect-recovery-file
    --expect "recovery file")

  # In a window shown without focus (M10a's desktop tests).
  _se_editor_desktop(play_session play_session --readback 200
    --replay $<TARGET_FILE:se_replay>)
  _se_editor_desktop(tick_fault tick_fault --expect-fault-files)
  _se_editor_desktop(render_fault render_fault --expect-fault-files)
  _se_editor_desktop(quit_save quit_save --scene-copy "${_se_sandbox}"
    --expect-scene-contains "321")
  _se_editor_desktop(dialog_quit dialog_quit
    --expect "1 answered during the quit")
endif()
