# The dedicated sim driver, headless (docs/milestone-specs.md M17a; label
# unit, and every one of them runs under TSan in linux-clang-tsan).
#
#   se_sim_tests          Catch2: SpscQueue, Doorbell, FaultBox, the
#                         PacedClock on a ManualClock (and the real clock,
#                         [slow]), the InputFolder's placement, the
#                         SnapshotChannel and its seeded property runs, and
#                         the SimThread: inline against threaded hashes on
#                         the kernel and rampage goldens and scenarios, the
#                         commands, a faulting tick (ADR-083), catch-up
#                         bursts, and the sim thread's snapshots against the
#                         inline driver's RenderLists (ADR-086); M13a: the
#                         stack's content gate and content bundles
#                         (content_gate_test.cc, ADR-123)

se_add_module(se_sim_tests KIND EXECUTABLE
  SOURCES
    content_gate_test.cc
    doorbell_test.cc
    fault_box_test.cc
    input_folder_test.cc
    paced_clock_test.cc
    sim_thread_test.cc
    snapshot_channel_test.cc
    spsc_queue_test.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    se_core
    se_render_core
    se_sim
    se_test_support
    se_content_test_support
    game_rampage
    rampage_test_support
    Catch2::Catch2WithMain
  DETERMINISTIC
  TEST)

set(_se_sim_out "${CMAKE_BINARY_DIR}/sim_out")
file(MAKE_DIRECTORY "${_se_sim_out}")
target_compile_definitions(se_sim_tests PRIVATE
  SE_TEST_OUT_DIR="${_se_sim_out}")

catch_discover_tests(se_sim_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "sim."
  ADD_TAGS_AS_LABELS
  PROPERTIES LABELS unit)
