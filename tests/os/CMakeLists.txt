# se_os and se_log tests (docs/milestone-specs.md, M4a "Tests"); label unit.
#
#   se_os_tests        Catch2: StartThread, RoleScope (OS names read back,
#                      QoS and CPU sets), the CPU topology tables, the
#                      unbiased clock, debugger detection, ExecutableDir, and
#                      the LogWorker (order across threads, drops, the rate
#                      limit, Stop, the synchronous fallback, the panel)
#   os.log_fatal.*     SE_LOG_FATAL with a running worker reaches the sinks
#                      before the abort (a death test, then a file check)

set(_se_os_out "${CMAKE_BINARY_DIR}/os_out")
file(MAKE_DIRECTORY "${_se_os_out}")

se_add_module(se_os_tests KIND EXECUTABLE
  SOURCES
    cpu_topology_test.cc
    log_worker_test.cc
    os_misc_test.cc
    thread_test.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    se_core
    se_log
    se_os
    se_test_support
    Catch2::Catch2WithMain
  TEST)

target_compile_definitions(se_os_tests PRIVATE
  SE_TEST_OUT_DIR="${_se_os_out}")

catch_discover_tests(se_os_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "os."
  ADD_TAGS_AS_LABELS
  PROPERTIES LABELS unit)

se_add_module(se_log_fatal_test KIND EXECUTABLE
  SOURCES
    log_fatal_main.cc
  PRIVATE_DEPS
    se_core
    se_log
    se_os
    se_test_support
  TEST)

set(_se_fatal_log "${_se_os_out}/fatal.log")
# The worker's own line format on stderr: the worker wrote it, not the
# synchronous fallback ("[F t=0 main] ...").
add_test(NAME os.log_fatal.abort
  COMMAND se_log_fatal_test fatal "${_se_fatal_log}")
set_tests_properties(os.log_fatal.abort PROPERTIES
  LABELS "unit;death"
  FIXTURES_SETUP se_log_fatal
  PASS_REGULAR_EXPRESSION
    "[0-9]+\\.[0-9]+ F main t=[0-9]+ log_fatal_main\\.cc:[0-9]+ fatal marker 42")
add_test(NAME os.log_fatal.file
  COMMAND se_log_fatal_test check "${_se_fatal_log}")
set_tests_properties(os.log_fatal.file PROPERTIES
  LABELS unit
  FIXTURES_REQUIRED se_log_fatal)
