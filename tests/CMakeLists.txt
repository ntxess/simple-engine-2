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
    support/kernel_session.cc
    support/kernel_test_components.cc
    support/kernel_test_module.cc
    support/null_wake_main.cc
    support/virtual_display.cc
  PUBLIC_DEPS
    se_core
    se_sim
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
    kernel/determinism_test.cc
    kernel/fixed_stepper_test.cc
    kernel/game_state_stack_test.cc
    kernel/guid_test.cc
    kernel/hash_test.cc
    kernel/log_test.cc
    kernel/parallel_view_test.cc
    kernel/replay_test.cc
    kernel/rng_test.cc
    kernel/schedule_test.cc
    kernel/serialization_test.cc
    kernel/session_log_test.cc
    kernel/sim_clock_test.cc
    kernel/thread_role_test.cc
    kernel/world_test.cc
  PRIVATE_DEPS
    se_core
    se_sim
    se_test_support
    Catch2::Catch2WithMain
  DETERMINISTIC
  TEST)

catch_discover_tests(se_tests
  DISCOVERY_MODE PRE_TEST
  ADD_TAGS_AS_LABELS
  PROPERTIES LABELS unit)

# glaze stays confined to reflect_glaze.h and *_components.cc (ADR-010).
add_test(NAME checks.glaze
  COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/checks/check_glaze.py"
          "${CMAKE_BINARY_DIR}/se_checks.json")
add_test(NAME checks.planted.glaze
  COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/checks/check_glaze.py"
          --planted "${CMAKE_SOURCE_DIR}/tools/checks/planted/glaze")
set_tests_properties(checks.glaze checks.planted.glaze PROPERTIES LABELS checks)

# Death tests: one executable, one CTest test per scenario; each must end in a
# failed SE_ASSERT. Release builds compile the asserts out.
se_add_module(se_death_tests KIND EXECUTABLE
  SOURCES
    death/death_main.cc
  PRIVATE_DEPS
    se_core
    se_test_support
  DETERMINISTIC
  TEST)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  foreach(_se_scenario
      world_access_in_fork structural_call_in_fork create_in_fork
      undeclared_read undeclared_write write_outside_chunk
      view_after_structural_change play_mutation_gate_closed
      access_from_another_thread duplicate_guid unsealed_registry)
    add_test(NAME death.${_se_scenario}
      COMMAND se_death_tests ${_se_scenario})
    set_tests_properties(death.${_se_scenario} PROPERTIES
      LABELS "unit;death"
      PASS_REGULAR_EXPRESSION "SE_ASSERT failed")
  endforeach()
endif()

# Compile-fail tests (clang only: the thread-safety analysis is clang's). The
# fixtures build in their own tree at test time.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_se_cf_dir "${CMAKE_BINARY_DIR}/compile_fail")
  add_test(NAME compile_fail.configure
    COMMAND "${CMAKE_COMMAND}"
            -S "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail"
            -B "${_se_cf_dir}"
            -G "${CMAKE_GENERATOR}"
            "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DSE_REPO_ROOT=${CMAKE_SOURCE_DIR}")
  set_tests_properties(compile_fail.configure PROPERTIES
    FIXTURES_SETUP se_compile_fail LABELS checks)
  add_test(NAME compile_fail.chunk_lambda_calls_create
    COMMAND "${CMAKE_COMMAND}" --build "${_se_cf_dir}"
            --target chunk_create_fails)
  set_tests_properties(compile_fail.chunk_lambda_calls_create PROPERTIES
    FIXTURES_REQUIRED se_compile_fail LABELS checks
    PASS_REGULAR_EXPRESSION "requires holding role 'g_sim_owner'")
  add_test(NAME compile_fail.control_compiles
    COMMAND "${CMAKE_COMMAND}" --build "${_se_cf_dir}"
            --target chunk_create_control)
  set_tests_properties(compile_fail.control_compiles PROPERTIES
    FIXTURES_REQUIRED se_compile_fail LABELS checks)
endif()
