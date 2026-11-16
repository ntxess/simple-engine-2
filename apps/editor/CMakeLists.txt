# simple_editor (docs/milestone-specs.md M10a, apps/editor): the editor
# shell (se_editor) on se_runtime's App with rampage's GameModule, opening
# rampage's sandbox scene by default. Its tests are tests/editor
# (scripts through --editor-script; label desktop for the windowed ones).
# crashpad_handler is copied next to it, like rampage's.

se_add_module(simple_editor KIND EXECUTABLE
  SOURCES
    main.cc
  PRIVATE_DEPS
    game_rampage
    se_editor
    se_runtime)
se_crash_copy_handler(simple_editor)
# Its textures load from the pages cooked next to it (M9).
add_dependencies(simple_editor se_cooked_textures)
