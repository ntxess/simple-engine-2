# se_replay: runs a game module headless and reports per-tick hashes
# (docs/milestone-specs.md, M2 "Tools and tests"; the rampage module, M3b).
# Tool code, so it links the test support libraries that hold the kernel test
# module and the rampage session. The rampage replay and reference tests are
# registered in tests/rampage/CMakeLists.txt.

se_add_module(se_replay KIND EXECUTABLE
  SOURCES
    main.cc
  PRIVATE_DEPS
    se_core
    se_sim
    se_test_support
    rampage_test_support
  TEST)

set(_se_replay_out "${CMAKE_BINARY_DIR}/se_replay_out")
file(MAKE_DIRECTORY "${_se_replay_out}")
set(_se_edits "${CMAKE_SOURCE_DIR}/tests/fixtures/kernel/edits.txt")

# The tool reproduces the determinism golden at 60 Hz with jitter.
add_test(NAME replay.kernel_golden_60hz
  COMMAND se_replay --module kernel_test --ticks 600 --edits "${_se_edits}"
          --fixed-frame-dt 16.6667 --jitter-ms 1.5
          --expect-golden determinism_kernel
          --hash-out "${_se_replay_out}/hashes_60hz.txt"
          --projection-out "${_se_replay_out}/projection_60hz.txt")
# Record at 144 Hz, then replay the recording at 30 Hz: every recorded hash
# must match.
add_test(NAME replay.kernel_record_144hz
  COMMAND se_replay --module kernel_test --ticks 450 --edits "${_se_edits}"
          --fixed-frame-dt 6.9444 --jitter-ms 1
          --record "${_se_replay_out}/kernel_144hz.sereplay")
add_test(NAME replay.kernel_replay_30hz
  COMMAND se_replay --module kernel_test
          --inputs "${_se_replay_out}/kernel_144hz.sereplay"
          --fixed-frame-dt 33.3333)
set_tests_properties(replay.kernel_record_144hz PROPERTIES
  FIXTURES_SETUP se_replay_recording)
set_tests_properties(replay.kernel_replay_30hz PROPERTIES
  FIXTURES_REQUIRED se_replay_recording)
# A replay carries its session log, so extra edits are rejected, not ignored.
add_test(NAME replay.edits_with_inputs_rejected
  COMMAND se_replay --module kernel_test
          --inputs "${_se_replay_out}/kernel_144hz.sereplay"
          --edits "${_se_edits}")
set_tests_properties(replay.edits_with_inputs_rejected PROPERTIES
  FIXTURES_REQUIRED se_replay_recording
  PASS_REGULAR_EXPRESSION "--edits cannot be combined with --inputs")
add_test(NAME replay.kernel_det_crosscheck
  COMMAND se_replay --module kernel_test --ticks 400 --edits "${_se_edits}"
          --fixed-frame-dt 6.25 --det-crosscheck 5 --jobs 3)
# A different seed must be reported as a golden mismatch.
add_test(NAME replay.kernel_golden_detects_change
  COMMAND se_replay --module kernel_test --ticks 600 --edits "${_se_edits}"
          --seed 43 --expect-golden determinism_kernel)
# It must check even when goldens are being rewritten: with
# SE_UPDATE_GOLDENS=1 it would store the seed-43 run as the golden.
set_tests_properties(replay.kernel_golden_detects_change PROPERTIES
  WILL_FAIL TRUE
  ENVIRONMENT_MODIFICATION "SE_UPDATE_GOLDENS=unset:")
set_tests_properties(replay.kernel_golden_60hz replay.kernel_record_144hz
  replay.kernel_replay_30hz replay.edits_with_inputs_rejected
  replay.kernel_det_crosscheck
  replay.kernel_golden_detects_change
  PROPERTIES LABELS "determinism;tools")
