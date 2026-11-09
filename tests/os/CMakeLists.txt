# se_os and se_log tests (docs/milestone-specs.md, M4a "Tests"); label unit.
#
#   se_os_tests   Catch2: StartThread, RoleScope (OS names read back, QoS and
#                 CPU sets), the CPU topology tables, the unbiased clock,
#                 debugger detection and ExecutableDir

se_add_module(se_os_tests KIND EXECUTABLE
  SOURCES
    cpu_topology_test.cc
    os_misc_test.cc
    thread_test.cc
  PRIVATE_DEPS
    se_core
    se_os
    se_test_support
    Catch2::Catch2WithMain
  TEST)

catch_discover_tests(se_os_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "os."
  ADD_TAGS_AS_LABELS
  PROPERTIES LABELS unit)
