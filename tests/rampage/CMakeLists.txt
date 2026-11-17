# game_rampage tests (docs/milestone-specs.md, M3b "Tests"); label rampage.
#
#   se_rampage_tests      Catch2: serialization, broadphase, effects, path,
#                         boundary, JSON, scenes, the legacy scenarios, the
#                         projection, sprites and the view, and
#                         determinism (golden determinism_rampage)
#   replay.rampage.*      se_replay runs the module (tools)
#   ref.sandbox.NAME      label reference: se_replay runs the legacy scenario
#                         NAME with its inputs for 3600 ticks, and
#                         tools/refcmp/refcmp.py compares the projection with
#                         tests/fixtures/reference/NAME.seproj.gz (the
#                         legacy-ref-v2 goldens, with rules R11 to R13)
#   ref.sandbox_v1.NAME   the same with --legacy-rules v1 against the
#                         legacy-ref-v1 goldens kept in
#                         tests/fixtures/reference/v1/

# game_rampage's link-graph contract: never SDL, ImGui, audio, crash or LZ4
# libraries (checked against every module's closure by se_check_link_graph).
get_target_property(_se_rampage_forbid game_rampage SE_FORBID)
foreach(_se_target SDL3::SDL3-static se_imgui miniaudio sentry lz4_static)
  if(NOT _se_target IN_LIST _se_rampage_forbid)
    message(FATAL_ERROR "game_rampage must FORBID ${_se_target}")
  endif()
endforeach()

set(_se_rampage_out "${CMAKE_BINARY_DIR}/rampage_out")
file(MAKE_DIRECTORY "${_se_rampage_out}")

se_add_module(rampage_test_support KIND STATIC
  SOURCES
    support/rampage_session.cc
    support/seinput.cc
  PUBLIC_DEPS
    game_rampage
    se_core
    se_sim
  DETERMINISTIC
  TEST)
target_include_directories(rampage_test_support PUBLIC
  "${CMAKE_SOURCE_DIR}/tests")
target_compile_definitions(rampage_test_support PRIVATE
  SE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

se_add_module(se_rampage_tests KIND EXECUTABLE
  SOURCES
    audio_events_test.cc
    boundary_test.cc
    broadphase_test.cc
    determinism_test.cc
    effects_test.cc
    json_test.cc
    legacy_scenario_test.cc
    path_test.cc
    prefab_test.cc
    projection_test.cc
    scene_test.cc
    serialization_test.cc
    view_test.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    game_rampage
    rampage_test_support
    se_core
    se_render_core
    se_sim
    se_test_support
    Catch2::Catch2WithMain
  DETERMINISTIC
  TEST)

target_compile_definitions(se_rampage_tests PRIVATE
  SE_TEST_OUT_DIR="${_se_rampage_out}")

catch_discover_tests(se_rampage_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "rampage."
  ADD_TAGS_AS_LABELS
  PROPERTIES LABELS rampage)

set(_se_ref "${CMAKE_SOURCE_DIR}/tests/fixtures/reference")
set(_se_refcmp "${CMAKE_SOURCE_DIR}/tools/refcmp/refcmp.py")

# Legacy comparisons: run, then compare (fixtures order the two).
function(_se_reference_test name dir rules golden)
  set(_out "${_se_rampage_out}/${name}.seproj")
  add_test(NAME ${name}.run
    COMMAND se_replay --module rampage
            --legacy-scenario "${dir}/${golden}.scenario.json"
            --inputs "${dir}/${golden}.seinput" --ticks 3600
            --legacy-rules ${rules} --projection-out "${_out}")
  add_test(NAME ${name}
    COMMAND Python3::Interpreter "${_se_refcmp}" "${_out}"
            "${dir}/${golden}.seproj.gz")
  set_tests_properties(${name}.run PROPERTIES
    FIXTURES_SETUP ${name} LABELS "reference;rampage")
  set_tests_properties(${name} PROPERTIES
    FIXTURES_REQUIRED ${name} LABELS "reference;rampage")
endfunction()

foreach(_se_name stress effects patrol)
  _se_reference_test(ref.sandbox.${_se_name} "${_se_ref}" v2 ${_se_name})
  _se_reference_test(ref.sandbox_v1.${_se_name} "${_se_ref}/v1" v1
                     ${_se_name})
endforeach()

# refcmp itself: a projection equals itself, and a changed stat is found;
# refcmp.planted pins each rule on planted projections (tools/refcmp/planted:
# the tolerance boundaries, rotation wrap, stat text, missing entities and
# ticks, and bad input).
add_test(NAME refcmp.self
  COMMAND Python3::Interpreter "${_se_refcmp}"
          "${_se_ref}/v1/effects.seproj.gz" "${_se_ref}/v1/effects.seproj.gz")
add_test(NAME refcmp.detects_difference
  COMMAND Python3::Interpreter "${_se_refcmp}"
          "${_se_ref}/v1/effects.seproj.gz" "${_se_ref}/v1/patrol.seproj.gz")
set_tests_properties(refcmp.detects_difference PROPERTIES
  PASS_REGULAR_EXPRESSION "FIRST DIFFERENCE")
add_test(NAME refcmp.planted
  COMMAND Python3::Interpreter
          "${CMAKE_SOURCE_DIR}/tools/refcmp/refcmp_test.py")
set_tests_properties(refcmp.self refcmp.detects_difference refcmp.planted
  PROPERTIES LABELS "tools;rampage")

# se_replay runs the module (this replaces M2's replay.rampage_not_yet).
add_test(NAME replay.rampage.sandbox_60hz
  COMMAND se_replay --module rampage --ticks 3600
          --fixed-frame-dt 16.6667 --jitter-ms 1.5
          --expect-golden determinism_rampage
          --hash-out "${_se_rampage_out}/sandbox_60hz.txt"
          --projection-out "${_se_rampage_out}/sandbox_60hz.seproj")
add_test(NAME replay.rampage.record_144hz
  COMMAND se_replay --module rampage --ticks 900
          --fixed-frame-dt 6.9444 --jitter-ms 1
          --record "${_se_rampage_out}/sandbox_144hz.sereplay")
add_test(NAME replay.rampage.replay_30hz
  COMMAND se_replay --module rampage
          --inputs "${_se_rampage_out}/sandbox_144hz.sereplay"
          --fixed-frame-dt 33.3333)
set_tests_properties(replay.rampage.record_144hz PROPERTIES
  FIXTURES_SETUP se_rampage_recording)
set_tests_properties(replay.rampage.replay_30hz PROPERTIES
  FIXTURES_REQUIRED se_rampage_recording)
add_test(NAME replay.rampage.bad_scene
  COMMAND se_replay --module rampage --scene "${_se_ref}/stress.seinput")
set_tests_properties(replay.rampage.bad_scene PROPERTIES
  PASS_REGULAR_EXPRESSION "se_replay: .*stress.seinput")
set_tests_properties(replay.rampage.sandbox_60hz replay.rampage.record_144hz
  replay.rampage.replay_30hz replay.rampage.bad_scene
  PROPERTIES LABELS "determinism;tools;rampage")
