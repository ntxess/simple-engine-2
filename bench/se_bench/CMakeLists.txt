# se_bench: the benchmark harness (docs/milestone-specs.md M3c). Suites are
# source files compiled into the executable; each registers itself with a
# SuiteRegistrar (se_bench/suite.h).
#
# Adding a suite from another module is one source file plus one line here:
#   se_bench_suite(<name> SOURCES suites/<name>.cc DEPS <module>)
# and an entry for <name> in bench/thresholds.json, which gives the CTest gate
# bench.<name> its scene size, iteration counts and p95 limit.

# Tool code like se_replay (TEST: exempt from the banned-API scan, so main may
# catch).
se_add_module(se_bench KIND EXECUTABLE
  SOURCES
    src/main.cc
    src/suite.cc
  PRIVATE_DEPS
    se_core
  FORBID
    SDL3::SDL3-static se_imgui miniaudio sentry lz4_static
  BANNED_INCLUDES
    ${SE_BAN_PLATFORM_INCLUDES}
  TEST)
target_compile_definitions(se_bench PRIVATE
  "SE_BENCH_CONFIG=\"$<CONFIG>\""
  "SE_BENCH_COMPILER=\"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\"")

set(_se_bench_thresholds "${CMAKE_SOURCE_DIR}/bench/thresholds.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_se_bench_thresholds}")
file(READ "${_se_bench_thresholds}" SE_BENCH_THRESHOLDS)
set(SE_BENCH_OUT "${CMAKE_BINARY_DIR}/bench_out")
file(MAKE_DIRECTORY "${SE_BENCH_OUT}")

# se_bench_suite(<name> SOURCES <file>... [DEPS <target>...])
#   Compiles the suite into se_bench, links what it measures, and adds
#   bench.<name> (label bench: the threshold gate from bench/thresholds.json,
#   run alone) and se_bench.smoke.<name> (labels unit and tools: a few
#   iterations of a small scene, so every preset runs the suite's code,
#   sanitizers included).
function(se_bench_suite name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPS")
  target_sources(se_bench PRIVATE ${arg_SOURCES})
  if(arg_DEPS)
    target_link_libraries(se_bench PRIVATE ${arg_DEPS})
  endif()
  foreach(_key n iterations warmup p95_ms)
    string(JSON _value ERROR_VARIABLE _error
      GET "${SE_BENCH_THRESHOLDS}" suites ${name} ${_key})
    if(_error)
      message(FATAL_ERROR
        "bench/thresholds.json has no suites.${name}.${_key}: measure the "
        "suite with the msvc-release preset and add its entry (${_error})")
    endif()
    set(_${_key} "${_value}")
  endforeach()
  add_test(NAME bench.${name}
    COMMAND se_bench --suite ${name} --n ${_n} --iterations ${_iterations}
            --warmup ${_warmup} --max-p95-ms ${_p95_ms}
            --json "${SE_BENCH_OUT}/${name}.json")
  set_tests_properties(bench.${name} PROPERTIES LABELS bench RUN_SERIAL TRUE)
  add_test(NAME se_bench.smoke.${name}
    COMMAND se_bench --suite ${name} --n 500 --iterations 3 --warmup 1
            --json "${SE_BENCH_OUT}/${name}.smoke.json")
  set_tests_properties(se_bench.smoke.${name} PROPERTIES
    LABELS "unit;tools")
endfunction()

se_bench_suite(renderlist_10k
  SOURCES suites/renderlist_10k.cc
  DEPS se_render_core)

add_test(NAME se_bench.list COMMAND se_bench --list)
set_tests_properties(se_bench.list PROPERTIES
  LABELS "unit;tools" PASS_REGULAR_EXPRESSION "renderlist_10k")
add_test(NAME se_bench.unknown_suite COMMAND se_bench --suite no_such_suite)
set_tests_properties(se_bench.unknown_suite PROPERTIES
  LABELS "unit;tools" PASS_REGULAR_EXPRESSION "unknown suite 'no_such_suite'")
