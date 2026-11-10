# The rampage executable (docs/milestone-specs.md, M5): game_rampage on
# se_runtime, the sandbox scene at 120 Hz with the clear-only frame. No
# ImGui. Its textures are the cooked pages of config/texture.json (M9),
# loaded through the background lane. Its desktop tests are in
# tests/platform/CMakeLists.txt, its M4 exit dumps in
# tests/crash/CMakeLists.txt. crashpad_handler is copied next to it: crash
# capture is its first boot step.

se_add_module(rampage KIND EXECUTABLE
  SOURCES
    main.cc
  PRIVATE_DEPS
    game_rampage
    se_runtime
  FORBID
    se_imgui)
se_crash_copy_handler(rampage)
# Its textures load from the pages cooked next to it (M9).
add_dependencies(rampage se_cooked_textures)
