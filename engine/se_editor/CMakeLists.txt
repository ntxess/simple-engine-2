# se_editor: the editor shell on the single loop (docs/milestone-specs.md
# M10a; ADR-095 to ADR-099). The panel interface (panel.h: Panel,
# PanelContext, EditorCommands, PlayEdits, EditorViewports), the commands
# and their undo (commands.h), the selection, TextureIds, EditorStats and
# LogView: the contract with M10b's panels (engine/se_editor/panels). Main
# thread only; not deterministic. Game modules reach it only through their
# GameModule (and the scene hooks the editor executable gives it), never
# by linking.

se_add_module(se_editor KIND STATIC
  SOURCES
    src/commands.cc
    src/json_value.cc
    src/log_view.cc
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
