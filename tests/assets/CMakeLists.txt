# The background lane and the AssetRegistry (docs/milestone-specs.md M9;
# test names start with "assets."). Label unit: they run everywhere, the
# WSL sanitizer presets included (TSan for the rings and the lane). The
# desktop hot-reload test is in tests/render_gpu (it needs rampage's
# window and GPU).
#
# The lane calls se_platform's real WakeMain(), which pushes nothing while
# no wake event is registered, so these binaries do not link
# se_test_support (which carries the null one).

set(_se_assets_out "${CMAKE_BINARY_DIR}/assets_out")
file(MAKE_DIRECTORY "${_se_assets_out}")

se_add_module(se_assets_tests KIND EXECUTABLE
  SOURCES
    atlas_test.cc
    lane_test.cc
    registry_test.cc
    support/lane_test_support.cc
    support/png.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    se_assets
    se_core
    se_platform
    se_runtime
    se_sim
    se::stb
    Catch2::Catch2WithMain
  TEST)
target_include_directories(se_assets_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/tests")
target_compile_definitions(se_assets_tests PRIVATE
  SE_TEST_OUT_DIR="${_se_assets_out}"
  SE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

catch_discover_tests(se_assets_tests
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "assets."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS unit
    TIMEOUT 120)
