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
  TEST)
# The local dump stream layout is included as "crash/probe_dump_stream.h".
target_include_directories(se_crash_probe PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/..")
se_crash_copy_handler(se_crash_probe)

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

# No in-process dump writing in engine code (tools/checks/check_banned_api.py,
# rule "dump"): the planted MiniDumpWriteDump call and <dbghelp.h> include
# must be found. (M4a extends the banned-API scan with DebugBreak and
# friends; the integration keeps one rule.)
add_test(NAME checks.planted.dump
  COMMAND Python3::Interpreter
          "${CMAKE_SOURCE_DIR}/tools/checks/check_banned_api.py"
          --planted "${CMAKE_SOURCE_DIR}/tools/checks/planted/dump")
set_tests_properties(checks.planted.dump PROPERTIES LABELS checks
  PASS_REGULAR_EXPRESSION "banned \\(dump\\)[^\n]*MiniDumpWriteDump")

# read_dump.py on synthetic minidumps (any platform).
add_test(NAME minidump.read_dump
  COMMAND Python3::Interpreter
          "${CMAKE_SOURCE_DIR}/tools/minidump/read_dump_test.py")
set_tests_properties(minidump.read_dump PROPERTIES LABELS tools)
