# The threaded ForkJoinPool (docs/milestone-specs.md M14); label unit, and
# every test runs under TSan in linux-clang-tsan.
#
#   se_jobs_tests     Catch2: the pool's properties (no worker until a site
#                     is enabled, exactly-once chunks, visibility, no lost
#                     wake, exceptions, stop, nested forks, per-chunk
#                     outputs, PerformanceOnly, ForkStats, the fallback
#                     window, the spin, calibration) for W = 1/3/7 and the
#                     chaos seeds, and determinism across W = 0/1/3/7 and
#                     eight chaos seeds: the kernel golden, rampage's
#                     sandbox golden and reference scenarios, pile-up
#                     scenes and the RenderList

se_add_module(se_jobs_tests KIND EXECUTABLE
  SOURCES
    determinism_test.cc
    pool_test.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    game_rampage
    rampage_test_support
    se_core
    se_os
    se_render_core
    se_sim
    se_test_support
    Catch2::Catch2WithMain
  DETERMINISTIC
  TEST)

catch_discover_tests(se_jobs_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "jobs."
  ADD_TAGS_AS_LABELS
  PROPERTIES LABELS unit)

# The wake-stall soak (ADR-110): se_wake_soak runs a pool of the machine's
# W_max workers at 120 frames a second with four forks each and records
# every wake's latency. jobs.wake_soak is the 10-minute run (labels slow and
# soak, Release builds only, alone on the machine); jobs.wake_soak_smoke runs
# it for 2 s everywhere (sanitizers included) without the bound.
se_add_module(se_wake_soak KIND EXECUTABLE
  SOURCES
    wake_soak_main.cc
  PRIVATE_DEPS
    se_core
    se_os
    se_test_support
  TEST)
set(_se_jobs_out "${CMAKE_BINARY_DIR}/jobs_out")
file(MAKE_DIRECTORY "${_se_jobs_out}")
add_test(NAME jobs.wake_soak_smoke
  COMMAND se_wake_soak --seconds 2 --max-wake-ms 1000
          --json "${_se_jobs_out}/wake_soak_smoke.json")
set_tests_properties(jobs.wake_soak_smoke PROPERTIES LABELS unit)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_test(NAME jobs.wake_soak
    COMMAND se_wake_soak --seconds 600 --max-wake-ms 2
            --json "${_se_jobs_out}/wake_soak.json")
  set_tests_properties(jobs.wake_soak PROPERTIES
    LABELS "slow;soak" RUN_SERIAL TRUE TIMEOUT 900)
endif()

# M14 exit: no fork site is enabled in any shipped configuration. The scan
# covers engine/, games/, apps/ and config/ (tests, benches and tools enable
# sites explicitly); the planted fixture proves it flags one.
add_test(NAME checks.no_fork_site_enabled
  COMMAND Python3::Interpreter
          "${CMAKE_CURRENT_SOURCE_DIR}/check_no_site_enabled.py"
          "${CMAKE_SOURCE_DIR}")
add_test(NAME checks.planted.fork_site_enabled
  COMMAND Python3::Interpreter
          "${CMAKE_CURRENT_SOURCE_DIR}/check_no_site_enabled.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/planted"
          "${CMAKE_CURRENT_SOURCE_DIR}/planted/engine")
set_tests_properties(checks.no_fork_site_enabled PROPERTIES LABELS checks)
set_tests_properties(checks.planted.fork_site_enabled PROPERTIES
  LABELS checks
  PASS_REGULAR_EXPRESSION "enable_site[.]cc:11: enabled_sites")
