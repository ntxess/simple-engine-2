# se_editor_panels: the editor's panels (docs/milestone-specs.md M10b).
#
# Panels derive from se_editor's Panel (panel.h, M10a), read a PanelContext
# (a const World and the frame's stats) and change the document only
# through EditorCommands. What needs no ImGui is kept in pure functions the
# tests drive directly: the waypoint canvas's controls and path-editing
# rules, the profiler's axis rule, picking, the stress
# generator's spawn plan and spreading, the viewport's letterbox math and
# the entity overlays.
#
# The panels sit over game_rampage (docs/design/costfree-end-state.md
# "Editor": se_editor is an application over se_runtime and game_rampage):
# the ported drawers edit its components (ADR-100). The link graph is closed
# by ALLOW_ONLY; the include scan keeps platform and GPU headers out.

se_add_module(se_editor_panels KIND STATIC
  SOURCES
    src/overlays.cc
    src/picking.cc
    src/profiler_series.cc
    src/stress_generator.cc
    src/viewport_math.cc
    src/waypoint_canvas.cc
  PUBLIC_DEPS
    game_rampage
    se_core
    se_render_core
    se_sim
  ALLOW_ONLY
    game_rampage
    se_core
    se_render_core
    se_sim
  BANNED_INCLUDES
    "^SDL3?/" "^windows\\.h$" "^Windows\\.h$" "^miniaudio" "^sentry"
    "^crashpad" "^lz4" "^SFML/" "^boost/")
