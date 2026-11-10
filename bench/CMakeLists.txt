# Benchmarks (docs/milestone-specs.md M3c): se_bench and its suites. The
# gates for the 7700X live in bench/thresholds.json; CTest runs them as
# bench.<suite> (label bench), which only the Release test presets include.

add_subdirectory(se_bench)

# M9: the background lane's steps (se_assets, se::fs: platform builds).
if(SE_BUILD_PLATFORM)
  add_subdirectory(lane_steps)
endif()
