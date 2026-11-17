# game_rampage tests (docs/milestone-specs.md, M3b "Tests"); label rampage.
#
#   se_rampage_tests      Catch2: serialization, broadphase, effects, path,
#                         boundary, JSON, scenes, the legacy scenarios, the
#                         projection, sprites and the view, and
#                         determinism (golden determinism_rampage); the
#                         game flow (rampage.flow.*, M13b) and its golden
#                         determinism_rampage_flow
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

# M13b: the game flow headless (FlowHarness, RunFlowSession) and the
# recorded session's script (StampedInput, from se_test_support).
se_add_module(rampage_flow_support KIND STATIC
  SOURCES
    support/flow_session.cc
  PUBLIC_DEPS
    game_rampage
    rampage_test_support
    se_core
    se_sim
    se_test_support
  DETERMINISTIC
  TEST)

se_add_module(se_rampage_tests KIND EXECUTABLE
  SOURCES
    audio_events_test.cc
    boundary_test.cc
    broadphase_test.cc
    determinism_test.cc
    effects_test.cc
    flow_determinism_test.cc
    flow_test.cc
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
    rampage_flow_support
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

# The flow's recorded session (tests/fixtures/flow/flow.seinput) is this
# generator's output; rampage.flow_input_fixture checks that it still is
# (newlines aside: the checkout may have CRLF).
se_add_module(rampage_flow_input_gen KIND EXECUTABLE
  SOURCES
    flow_input_gen.cc
  PRIVATE_DEPS
    rampage_flow_support
  TEST)
add_test(NAME rampage.flow_input_fixture
  COMMAND Python3::Interpreter -c
          "import sys,subprocess;subprocess.run([sys.argv[1],sys.argv[2]],check=True);a=open(sys.argv[2]).read();b=open(sys.argv[3]).read();print('same' if a==b else 'DIFFERENT');sys.exit(a!=b)"
          $<TARGET_FILE:rampage_flow_input_gen>
          "${_se_rampage_out}/flow.seinput"
          "${CMAKE_SOURCE_DIR}/tests/fixtures/flow/flow.seinput")
set_tests_properties(rampage.flow_input_fixture PROPERTIES
  LABELS "rampage;tools")

# M13b: the game flow through the executable (flow_app_test.py): the keys
# pushed with the --push-key test hook (SDL events, never OS input; not in
# Release) on the doomed scene: Title, Gameplay, Pause, Gameplay, the
# player's death, GameOver, Title. rampage.headless_flow records the session
# and se_replay reproduces its hashes from the replay's content bundle;
# rampage.desktop.flow reads the composite back in each state in an
# unfocused window (label desktop).
if(TARGET rampage AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  set(_se_flow_app "${CMAKE_CURRENT_SOURCE_DIR}/flow_app_test.py")
  add_test(NAME rampage.headless_flow
    COMMAND Python3::Interpreter "${_se_flow_app}"
            --rampage $<TARGET_FILE:rampage>
            --se-replay $<TARGET_FILE:se_replay>
            --mode headless --out "${_se_rampage_out}/headless_flow")
  set_tests_properties(rampage.headless_flow PROPERTIES
    LABELS "unit;rampage" TIMEOUT 120)
  add_test(NAME rampage.desktop.flow
    COMMAND Python3::Interpreter "${_se_flow_app}"
            --rampage $<TARGET_FILE:rampage>
            --mode desktop --out "${_se_rampage_out}/desktop_flow")
  set_tests_properties(rampage.desktop.flow PROPERTIES
    LABELS "desktop;gpu;rampage" TIMEOUT 120)
endif()

# M11/M13 integration: a scene the editor saved with prefab references
# plays in rampage (--start-state gameplay --scene, through the lane), and
# its first 600 tick hashes equal se_replay's on the editor's Play-start
# bytes of the same document (editor_saved_test.py; headless).
if(TARGET rampage AND TARGET simple_editor AND
   NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  add_test(NAME rampage.plays_an_editor_saved_scene
    COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/editor_saved_test.py"
            --editor $<TARGET_FILE:simple_editor>
            --rampage $<TARGET_FILE:rampage>
            --se-replay $<TARGET_FILE:se_replay>
            --content "${CMAKE_SOURCE_DIR}"
            --script
            "${CMAKE_SOURCE_DIR}/tests/editor/scripts/saved_for_rampage.editor"
            --out "${_se_rampage_out}/editor_saved")
  set_tests_properties(rampage.plays_an_editor_saved_scene PROPERTIES
    LABELS "unit;rampage" TIMEOUT 180)
endif()

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
