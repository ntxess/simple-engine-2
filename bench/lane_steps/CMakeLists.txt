# bench.lane_steps (docs/milestone-specs.md M9): the background lane's
# bounded steps (a 1 MB read, a 256 KB LZ4 block decode, a scene chunk
# parse), each gated on its p99 from bench/thresholds.json (suites.
# lane_steps: iterations, warmup, p99_ms). A separate executable: se_bench
# may not link SDL or LZ4, and the lane's reads go through se::fs.
#
#   bench.lane_steps            label bench (the Release test preset), run
#                               alone
#   se_bench.smoke.lane_steps   labels unit and tools: a few steps of each
#                               kind, so every preset runs the code

se_add_module(se_lane_bench KIND EXECUTABLE
  SOURCES
    main.cc
  PRIVATE_DEPS
    se_assets
    se_core
    se_platform
  TEST)
target_compile_definitions(se_lane_bench PRIVATE
  "SE_BENCH_CONFIG=\"$<CONFIG>\"")

set(_se_thresholds "${CMAKE_SOURCE_DIR}/bench/thresholds.json")
file(READ "${_se_thresholds}" _se_thresholds_json)
foreach(_key iterations warmup p99_ms)
  string(JSON _value ERROR_VARIABLE _error
    GET "${_se_thresholds_json}" suites lane_steps ${_key})
  if(_error)
    message(FATAL_ERROR
      "bench/thresholds.json has no suites.lane_steps.${_key} (${_error})")
  endif()
  set(_se_lane_${_key} "${_value}")
endforeach()
set(_se_lane_out "${CMAKE_BINARY_DIR}/bench_out")
file(MAKE_DIRECTORY "${_se_lane_out}")
add_test(NAME bench.lane_steps
  COMMAND se_lane_bench --iterations ${_se_lane_iterations}
          --warmup ${_se_lane_warmup} --max-p99-ms ${_se_lane_p99_ms}
          --json "${_se_lane_out}/lane_steps.json"
          --scratch "${_se_lane_out}")
set_tests_properties(bench.lane_steps PROPERTIES LABELS bench RUN_SERIAL TRUE)
add_test(NAME se_bench.smoke.lane_steps
  COMMAND se_lane_bench --iterations 5 --warmup 1
          --json "${_se_lane_out}/lane_steps.smoke.json"
          --scratch "${_se_lane_out}")
set_tests_properties(se_bench.smoke.lane_steps PROPERTIES
  LABELS "unit;tools")
