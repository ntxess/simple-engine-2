# se_editor_panels: the editor's panels (docs/milestone-specs.md M10b;
# ADR-100 to ADR-103).
#
# Panels derive from se_editor's Panel (panel.h, M10a), read a PanelContext
# (a const World and the frame's stats) and change the document only
# through EditorCommands (Edit mode) or PlayEdits (Play). What needs no
# ImGui is kept in pure functions the tests drive directly: the waypoint
# canvas's controls and path-editing rules, the profiler's axis rule,
# picking, the stress generator's spawn plan and spreading, the viewport's
# letterbox math, the gizmo and the entity overlays.
#
# The panels sit over game_rampage (docs/design/costfree-end-state.md
# "Editor": the editor is an application over se_runtime and game_rampage):
# the ported drawers, the canvas and the overlays edit and read its
# components (ADR-100). se_editor itself stays game-agnostic (it FORBIDs
# game_rampage); this module is where the editor meets the game. The link
# graph is closed by ALLOW_ONLY; the include scan keeps platform headers
# out of the panels' own sources.

se_add_module(se_editor_panels KIND STATIC
  SOURCES
    src/component_drawers.cc
    src/gizmo.cc
    src/hierarchy_panel.cc
    src/inspector_panel.cc
    src/log_panel.cc
    src/overlays.cc
    src/panel_support.cc
    src/path_commands.cc
    src/picking.cc
    src/profiler_panel.cc
    src/profiler_series.cc
    src/scene_settings_panel.cc
    src/stress_generator.cc
    src/stress_panel.cc
    src/viewport_math.cc
    src/viewport_panel.cc
    src/waypoint_canvas.cc
  PUBLIC_DEPS
    game_rampage
    se_core
    se_editor
    se_imgui
    se_render_core
    se_sim
  ALLOW_ONLY
    game_rampage
    se_core
    se_editor
    se_imgui
    se_render_core
    se_sim
  FORBID
    sentry
  BANNED_INCLUDES
    "^SDL3?/" "^windows\\.h$" "^Windows\\.h$" "^miniaudio" "^sentry"
    "^crashpad" "^lz4" "^SFML/" "^boost/")
