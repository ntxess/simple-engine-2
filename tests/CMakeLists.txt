# Engine tests (Catch2 v3 under CTest).
#
# Labels: unit, determinism, golden, slow, checks, lint, legacy, desktop, gpu.

se_use_deps(catch2)
include("${catch2_SOURCE_DIR}/extras/Catch.cmake")

se_add_module(se_tests KIND EXECUTABLE
  SOURCES
    smoke/smoke_test.cc
  PRIVATE_DEPS
    se_core
    Catch2::Catch2WithMain
  DETERMINISTIC
  TEST)

catch_discover_tests(se_tests
  DISCOVERY_MODE PRE_TEST
  ADD_TAGS_AS_LABELS
  PROPERTIES LABELS unit)
