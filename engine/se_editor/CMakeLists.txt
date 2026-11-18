# se_editor: the editor shell on the single loop (docs/milestone-specs.md
# M10a; ADR-095 to ADR-099). The panel interface (panel.h: Panel,
# PanelContext, EditorCommands, PlayEdits, EditorViewports), the commands
# and their undo (commands.h), the selection, TextureIds, EditorStats and
# LogView: the contract with M10b's panels (engine/se_editor/panels); the
# EditDocument, the PlaySession (Play, Play game, Play.Faulted, whose tick
# and render build are a fault boundary: src/play_fault_boundary.cc), the
# DialogMailbox, recovery and autosave, the test script, and the Editor:
# the App's AppExtension that runs it all on the single loop. Main thread
# only; not deterministic. Game modules reach it only through their
# GameModule (and the scene hooks the editor executable gives it), never
# by linking.

se_add_module(se_editor KIND STATIC
  SOURCES
    src/commands.cc
    src/dialog_mailbox.cc
    src/documents.cc
    src/edit_document.cc
    src/edit_queue.cc
    src/editor.cc
    src/editor_actions.cc
    src/editor_documents.cc
    src/editor_dump.cc
    src/editor_reload.cc
    src/editor_script.cc
    src/editor_ui.cc
    src/json_value.cc
    src/log_view.cc
    src/play_fault_boundary.cc
    src/play_session.cc
    src/play_states.cc
    src/prefab_commands.cc
    src/recovery.cc
    src/selection.cc
  PUBLIC_DEPS
    se_core
    se_sim
    se_runtime
    se_imgui
    se_render
    se_assets
    se_audio
  FORBID
    game_rampage sentry)
# Internal headers are included as "src/<name>.h".
target_include_directories(se_editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
