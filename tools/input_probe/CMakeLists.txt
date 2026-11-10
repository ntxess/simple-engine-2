# input_probe: a small window that logs each real key event's SDL stamp and
# reports the stamps' resolution, with SDL's raw keyboard on (the runtime's
# default) or off (Win32 message stamps, the editor's choice)
# (docs/milestone-specs.md M8 "Input"). Running it needs a person at the
# keyboard: docs/gates.md "Win32 key stamp resolution". Tool code (TEST:
# exempt from the banned-API scan); only its --help runs unattended.

se_add_module(input_probe KIND EXECUTABLE
  SOURCES
    main.cc
  PRIVATE_DEPS
    SDL3::SDL3-static
  TEST)

if(SE_BUILD_TESTS)
  add_test(NAME tools.input_probe.usage COMMAND input_probe --help)
  set_tests_properties(tools.input_probe.usage PROPERTIES
    LABELS tools PASS_REGULAR_EXPRESSION "usage: input_probe")
endif()
