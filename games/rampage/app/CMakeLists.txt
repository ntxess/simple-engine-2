# The rampage executable (docs/milestone-specs.md, M5): game_rampage on
# se_runtime, the sandbox scene at 120 Hz with the clear-only frame. No
# ImGui. Its desktop tests are in tests/platform/CMakeLists.txt.

se_add_module(rampage KIND EXECUTABLE
  SOURCES
    main.cc
  PRIVATE_DEPS
    game_rampage
    se_runtime
  FORBID
    se_imgui)
