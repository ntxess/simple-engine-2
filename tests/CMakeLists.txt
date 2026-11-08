# Engine tests (Catch2 v3 under CTest).
#
# Labels: unit, determinism, golden, slow, checks, lint, legacy, desktop, gpu.
# Catch2 tags become labels, so a test tagged [slow] is skipped by the
# msvc-debug preset.

se_use_deps(catch2)
include("${catch2_SOURCE_DIR}/extras/Catch.cmake")

# Goldens are kept per platform and compiler (tests/golden/<name>/).
if(WIN32)
  set(_se_golden_os windows)
elseif(APPLE)
  set(_se_golden_os macos)
else()
  set(_se_golden_os linux)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(_se_golden_cc msvc)
elseif(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(_se_golden_cc clangcl)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_se_golden_cc clang)
else()
  set(_se_golden_cc gcc)
endif()
set(SE_GOLDEN_PLATFORM "${_se_golden_os}-${_se_golden_cc}")

# Shared test support: goldens, role scopes and the null WakeMain().
se_add_module(se_test_support KIND STATIC
  SOURCES
    support/golden.cc
    support/null_wake_main.cc
    support/virtual_display.cc
  PUBLIC_DEPS
    se_core
  DETERMINISTIC
  TEST)
target_include_directories(se_test_support PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(se_test_support PUBLIC
  SE_GOLDEN_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/golden"
  SE_GOLDEN_PLATFORM="${SE_GOLDEN_PLATFORM}"
  SE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

se_add_module(se_tests KIND EXECUTABLE
  SOURCES
    smoke/smoke_test.cc
    kernel/detmath_test.cc
    kernel/fixed_stepper_test.cc
    kernel/guid_test.cc
    kernel/hash_test.cc
    kernel/log_test.cc
    kernel/rng_test.cc
    kernel/sim_clock_test.cc
    kernel/thread_role_test.cc
  PRIVATE_DEPS
    se_core
    se_test_support
    Catch2::Catch2WithMain
  DETERMINISTIC
  TEST)

catch_discover_tests(se_tests
  DISCOVERY_MODE PRE_TEST
  ADD_TAGS_AS_LABELS
  PROPERTIES LABELS unit)
