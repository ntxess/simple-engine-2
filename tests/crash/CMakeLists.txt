# Crash capture tests (docs/milestone-specs.md, M4b; label crash).
#
# se_crash_probe registers dump streams with known contents and then
# crashes or requests dumps; crash_test.py runs it with a fresh database,
# finds the dumps the handler wrote, decodes them with
# tools/minidump/read_dump.py and checks the exception record, the threads,
# the annotations and every stream. The crash tests need the crashpad
# backend (Windows; msvc-asan and Linux build the stub, ADR-034), where the
# probe checks instead that Start fails cleanly and the process runs on.

se_add_module(se_crash_probe KIND EXECUTABLE
  SOURCES
    crash_probe.cc
  PRIVATE_DEPS
    se_core
    se_crash
    se_os
  TEST)
se_crash_copy_handler(se_crash_probe)

# The watchdog skeleton and its HangDetector (label unit; every platform).
se_add_module(se_crash_tests KIND EXECUTABLE
  SOURCES
    watchdog_test.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    se_core
    se_crash
    se_os
    Catch2::Catch2WithMain
  TEST)
catch_discover_tests(se_crash_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "crash_unit."
  ADD_TAGS_AS_LABELS
  PROPERTIES LABELS unit)

set(_se_crash_test "${CMAKE_CURRENT_SOURCE_DIR}/crash_test.py")
set(_se_read_dump "${CMAKE_SOURCE_DIR}/tools/minidump/read_dump.py")
set(_se_crash_work "${CMAKE_BINARY_DIR}/crash_out")

function(_se_crash_test name)
  add_test(NAME crash.${name}
    COMMAND Python3::Interpreter "${_se_crash_test}"
            --probe "$<TARGET_FILE:se_crash_probe>"
            --read-dump "${_se_read_dump}"
            --work "${_se_crash_work}/${name}"
            --scenario ${name})
  set_tests_properties(crash.${name} PROPERTIES
    LABELS crash TIMEOUT 60 ENVIRONMENT "SE_NO_MESSAGE_BOX=1")
endfunction()

get_property(_se_crash_backend GLOBAL PROPERTY SE_CRASH_BACKEND)
if(_se_crash_backend STREQUAL "crashpad")
  foreach(_se_scenario access_violation abort terminate terminate_noexcept
                       stack_overflow dump_without_crash missing_handler
                       bad_handler)
    _se_crash_test(${_se_scenario})
  endforeach()
else()
  # The stub: Start returns an Error and the probe runs on.
  _se_crash_test(stub)
  set_tests_properties(crash.stub PROPERTIES LABELS unit)
endif()

# The M4 exit dumps through the real stack (docs/milestone-specs.md,
# "Integration (M4a + M4b + M5)"): rampage --crash-test after-ticks:240 and
# --dump-test after-ticks:240, each dump decoded with read_dump.py and
# checked for main's breadcrumb at tick 240, the log tail with the line
# logged just before and the InputFrame ring up to that tick; and a fault
# caught by the SDL_AppIterate boundary (--inject-throw), whose dump main
# requests. Labels desktop (rampage opens its window, without activating
# it; scripts/ci.ps1 -Desktop runs them) and crash. Test builds only:
# Release has no test hooks.
if(TARGET rampage AND _se_crash_backend STREQUAL "crashpad" AND
   NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  foreach(_se_scenario crash dump fault)
    add_test(NAME crash.rampage_${_se_scenario}
      COMMAND Python3::Interpreter
              "${CMAKE_CURRENT_SOURCE_DIR}/rampage_dump_test.py"
              --rampage "$<TARGET_FILE:rampage>"
              --read-dump "${_se_read_dump}"
              --work "${_se_crash_work}/rampage_${_se_scenario}"
              --scenario ${_se_scenario})
    set_tests_properties(crash.rampage_${_se_scenario} PROPERTIES
      LABELS "desktop;crash;rampage" TIMEOUT 90
      ENVIRONMENT "SE_NO_MESSAGE_BOX=1")
  endforeach()
  # M12: --hang-test after-ticks:240 gives an out-of-process hang dump
  # within 6 s that se_replay --from-dump replays to tick 240; a 6.5 s stall
  # inside a driver wait is labelled a GPU or compositor stall, its end is
  # logged and rampage quits on its own (rampage_hang_test.py).
  foreach(_se_scenario hang driver_stall)
    add_test(NAME crash.rampage_${_se_scenario}
      COMMAND Python3::Interpreter
              "${CMAKE_CURRENT_SOURCE_DIR}/rampage_hang_test.py"
              --rampage "$<TARGET_FILE:rampage>"
              --se-replay "$<TARGET_FILE:se_replay>"
              --read-dump "${_se_read_dump}"
              --work "${_se_crash_work}/rampage_${_se_scenario}"
              --scenario ${_se_scenario})
    set_tests_properties(crash.rampage_${_se_scenario} PROPERTIES
      LABELS "desktop;crash;rampage" TIMEOUT 120
      ENVIRONMENT "SE_NO_MESSAGE_BOX=1")
  endforeach()
  # The integration of M12 and M10: a hang inside a tick of the editor's
  # Play gives a dump within 6 s that carries the Play-start stream, and
  # se_replay --from-dump replays the session, its edits included, to the
  # hanging tick (the same script, with the editor executable).
  if(TARGET simple_editor)
    add_test(NAME crash.editor_play_hang
      COMMAND Python3::Interpreter
              "${CMAKE_CURRENT_SOURCE_DIR}/rampage_hang_test.py"
              --editor "$<TARGET_FILE:simple_editor>"
              --script "${CMAKE_SOURCE_DIR}/tests/editor/scripts/play_hang.editor"
              --se-replay "$<TARGET_FILE:se_replay>"
              --read-dump "${_se_read_dump}"
              --work "${_se_crash_work}/editor_play_hang"
              --scenario editor_play_hang)
    set_tests_properties(crash.editor_play_hang PROPERTIES
      LABELS "desktop;crash;editor" TIMEOUT 120
      ENVIRONMENT "SE_NO_MESSAGE_BOX=1")
  endif()
endif()

# read_dump.py on synthetic minidumps (any platform).
# M13a: a game's dump carries its SessionLog. content_probe runs the content
# test module as a game (its texts through the lane and the content gate);
# a focus loss logged as a state command, then a --dump-test dump, which
# se_replay --from-dump replays from the dump's session stream (a content
# bundle and the SessionLog) to its tick (session_log_dump_test.py).
se_add_module(content_probe KIND EXECUTABLE
  SOURCES
    content_probe.cc
  PRIVATE_DEPS
    se_runtime
    se_content_test_support
  FORBID
    se_imgui
  TEST)
se_crash_copy_handler(content_probe)
if(_se_crash_backend STREQUAL "crashpad" AND
   NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  add_test(NAME crash.runtime_dump_with_session_log
    COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/session_log_dump_test.py"
            --probe "$<TARGET_FILE:content_probe>"
            --se-replay "$<TARGET_FILE:se_replay>"
            --read-dump "${_se_read_dump}"
            --content-root "${CMAKE_SOURCE_DIR}/tests/fixtures"
            --work "${_se_crash_work}/runtime_dump_with_session_log")
  set_tests_properties(crash.runtime_dump_with_session_log PROPERTIES
    LABELS "desktop;crash" TIMEOUT 120 ENVIRONMENT "SE_NO_MESSAGE_BOX=1")
endif()

add_test(NAME minidump.read_dump
  COMMAND Python3::Interpreter
          "${CMAKE_SOURCE_DIR}/tools/minidump/read_dump_test.py")
set_tests_properties(minidump.read_dump PROPERTIES LABELS tools)
