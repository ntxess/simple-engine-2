# Editor tests (docs/milestone-specs.md M10a "Tests (M10a)" and
# "Integration (M10a + M10b + M12 + M14)"; test names start with
# "editor."). Headless everywhere: ImGui frames without a window
# (se_imgui's HeadlessFrames), documents and commands on rampage's
# registry, Play on the editor's sessions and their dump streams, the
# dialog mailbox and the lane (label unit; TSan in WSL where threads are
# involved). The script tests run the simple_editor executable, with the
# panels of se_editor_panels docked in it, headless and in a window (label
# desktop, tests/editor/*.py). The hang in editor Play is
# crash.editor_play_hang (tests/crash).

se_add_module(se_editor_tests KIND EXECUTABLE
  SOURCES
    commands_test.cc
    dialog_mailbox_test.cc
    document_test.cc
    documents_test.cc
    idle_test.cc
    imgui_context_test.cc
    play_dump_test.cc
    play_session_test.cc
    prefab_document_test.cc
    recovery_test.cc
    script_test.cc
    ../support/no_dialogs_listener.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    se_editor
    se_editor_panels
    se_imgui
    game_rampage
    rampage_test_support
    se_core
    se_replay_dump
    se_runtime
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

  # The panels in the editor (the M10a + M10b + M12 + M14 integration):
  # every panel draws, an edit is a command, Play registers the Play-start
  # dump stream and lists the fork sites, Stop leaves the document's hash.
  _se_editor_script(panels panels --replay $<TARGET_FILE:se_replay>
    --expect "editor: dock layout seeded (8 panels)")
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
  # File > Close of a dirty document waits for the unsaved-changes prompt:
  # Cancel keeps the changes, Save saves them first, Don't Save discards.
  _se_editor_script(close_dirty close_dirty --scene-copy "${_se_sandbox}"
    --expect-scene-contains "321" --expect "the close waits for the prompt")
  # An autosave that lands after a newer save leaves no recovery file.
  _se_editor_script(autosave_after_save autosave_after_save
    --scene-copy "${_se_sandbox}" --expect-scene-contains "222")
  # A failed script fails the process (its exit status).
  _se_editor_script(failure_exit_status failing --expect-script-failure)
  _se_editor_script(autosave_recovers autosave --scene-copy "${_se_sandbox}"
    --then-script "${_se_scripts}/recover.editor"
    --expect "editor: recovered")
  _se_editor_script(terminate_writes_recovery terminate
    --scene-copy "${_se_sandbox}" --expect-recovery-file
    --expect "recovery file")

  # M11b: several documents, prefabs, hot reload, stat names, the stress
  # generator's prefab instances. --content-copy runs the editor on a copy
  # of assets/ and config/, so saves and write-file never touch the
  # repository.
  set(_se_content "${CMAKE_SOURCE_DIR}")
  _se_editor_script(prefabs prefabs --content-copy "${_se_content}"
    --expect-file-contains assets/prefabs/enemy.prefab.json "250"
    --expect-file-contains assets/scenes/sandbox.scene.json
      "prefabs/enemy.prefab.json"
    --expect-file-contains assets/scenes/sandbox.scene.json "null"
    --expect-file-lacks assets/scenes/sandbox.scene.json "\"components\""
    --expect "follows it in one undo step")
  _se_editor_script(documents documents --content-copy "${_se_content}"
    --expect "the close waits for the prompt")
  _se_editor_script(hot_reload hot_reload --content-copy "${_se_content}"
    --expect "reloaded as document" --expect "asking (Reload, Keep mine)"
    --expect "handled at Stop")
  _se_editor_script(stat_names stat_names --scene-copy "${_se_sandbox}"
    --expect-scene-contains "Armor")
  _se_editor_script(stress_prefabs stress_prefabs
    --content-copy "${_se_content}"
    --expect-file-count assets/scenes/sandbox.scene.json
      "prefabs/enemy.prefab.json" 36
    --expect-file-lacks assets/scenes/sandbox.scene.json "\"components\"")

  # In a window shown without focus, the panels docked in it. The World is
  # drawn into the Viewport panel's ViewportTarget, so --readback reads that
  # target (the composite behind the UI is the clear colour).
  _se_editor_desktop(panels panels --readback 200
    --replay $<TARGET_FILE:se_replay>
    --expect "editor: dock layout seeded (8 panels)"
    --expect "editor: viewport 'scene'")
  _se_editor_desktop(play_session play_session --readback 200
    --replay $<TARGET_FILE:se_replay>)
  # 'Play game' in the view: keys pushed while the Viewport panel has the
  # focus reach the game's InputService and its replay (ADR-113).
  _se_editor_desktop(play_game_input play_game_input
    --replay $<TARGET_FILE:se_replay> --expect-edges 4
    --expect "play game: the view has the input")
  # The prefab script in a window: the Viewport's readback changes after
  # the enemy prefab's sprite region change is saved (M11b).
  _se_editor_desktop(prefabs prefabs_window --content-copy "${_se_content}"
    --readback-frames 60 450 --expect "follows it in one undo step")
  _se_editor_desktop(tick_fault tick_fault --expect-fault-files)
  _se_editor_desktop(render_fault render_fault --expect-fault-files)
  _se_editor_desktop(quit_save quit_save --scene-copy "${_se_sandbox}"
    --expect-scene-contains "321")
  _se_editor_desktop(dialog_quit dialog_quit
    --expect "1 answered during the quit")
  # M12 beside M10's Idle: an editor that waits for events is not a hang
  # (a 200 ms watchdog period, 4 s of Idle, then WM_CLOSE; Windows only:
  # the test finds the editor's window with user32).
  if(WIN32)
    add_test(NAME editor.desktop.idle_watchdog
      COMMAND Python3::Interpreter
              "${CMAKE_CURRENT_SOURCE_DIR}/idle_watchdog_test.py"
              --editor $<TARGET_FILE:simple_editor>
              --script "${_se_scripts}/idle.editor"
              --work "${CMAKE_CURRENT_BINARY_DIR}/idle_watchdog")
    set_tests_properties(editor.desktop.idle_watchdog PROPERTIES
      LABELS desktop TIMEOUT 120
      ENVIRONMENT "SE_NO_MESSAGE_BOX=1")
  endif()
endif()
