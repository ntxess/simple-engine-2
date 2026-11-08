# Tests of the legacy behaviour reference (label "legacy"), added by cmake/Legacy.cmake.
#
# legacy_test.py runs legacy_replay from the repository root, because the legacy
# tree reads config/ and assets/ relative to the working directory. Scenarios
# live in tests/fixtures/reference/ as NAME.scenario.json plus NAME.seinput.
#
#   legacy.selfdet.NAME        two runs and runs at --fps 30/60/144/160 give
#                              the same projection hash (docs/milestone-specs.md,
#                              M1a exit), and so does --fps 10, where every
#                              frame hits the 8-tick cap; each run renders the
#                              number of frames its pacing implies
#   legacy.selfdet.live_replay a paced run in a visible window, recorded, then
#                              replayed headless, gives the same hash. Label
#                              "desktop" too: it needs an interactive desktop,
#                              so the legacy test presets exclude it; run it by
#                              hand (docs/gates.md):
#                              ctest --test-dir out/build/legacy-msvc-debug -L desktop
#   legacy.record_replay.NAME  headless: replaying NAME.seinput with --record
#                              records exactly the frames it replayed (non-zero
#                              held, pressed and released masks), and replaying
#                              the recording gives the same hash
#   legacy.reference.NAME      every tick's projection hash equals the golden
#                              NAME.sehash and the projection equals the golden
#                              NAME.seproj.gz (generated at legacy-ref-v1); a
#                              declared rule (docs/rules/gameplay-rules.md)
#                              re-baselines both (scripts/ci.ps1 clears the
#                              variable, so the commit gate always checks):
#                              SE_UPDATE_GOLDENS=1 ctest --preset legacy-msvc-debug -R reference

set(_legacy_driver "${CMAKE_CURRENT_SOURCE_DIR}/legacy_test.py")
set(_legacy_fixtures "${CMAKE_SOURCE_DIR}/tests/fixtures/reference")
set(_legacy_work "${CMAKE_CURRENT_BINARY_DIR}/work")
set(_legacy_scenarios stress effects patrol)

foreach(_scenario IN LISTS _legacy_scenarios)
  add_test(NAME legacy.selfdet.${_scenario}
    COMMAND "${Python3_EXECUTABLE}" "${_legacy_driver}" selfdet
            --exe "$<TARGET_FILE:legacy_replay>"
            --scenario "${_legacy_fixtures}/${_scenario}"
            --work "${_legacy_work}/selfdet/${_scenario}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(legacy.selfdet.${_scenario} PROPERTIES LABELS legacy)

  add_test(NAME legacy.record_replay.${_scenario}
    COMMAND "${Python3_EXECUTABLE}" "${_legacy_driver}" record-replay
            --exe "$<TARGET_FILE:legacy_replay>"
            --scenario "${_legacy_fixtures}/${_scenario}"
            --work "${_legacy_work}/record_replay/${_scenario}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(legacy.record_replay.${_scenario} PROPERTIES
    LABELS legacy)

  add_test(NAME legacy.reference.${_scenario}
    COMMAND "${Python3_EXECUTABLE}" "${_legacy_driver}" reference
            --exe "$<TARGET_FILE:legacy_replay>"
            --scenario "${_legacy_fixtures}/${_scenario}"
            --work "${_legacy_work}/reference"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(legacy.reference.${_scenario} PROPERTIES LABELS legacy)
endforeach()

# Opens a visible window for about five seconds; keys pressed meanwhile are
# recorded and replayed, so the test holds either way. Not run by the legacy
# test presets (label "desktop"); see docs/gates.md.
add_test(NAME legacy.selfdet.live_replay
  COMMAND "${Python3_EXECUTABLE}" "${_legacy_driver}" live-replay
          --exe "$<TARGET_FILE:legacy_replay>"
          --scenario "${_legacy_fixtures}/stress"
          --work "${_legacy_work}/live_replay"
          --ticks 600 --fps 60
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(legacy.selfdet.live_replay PROPERTIES
  LABELS "legacy;desktop"
  RUN_SERIAL TRUE)
