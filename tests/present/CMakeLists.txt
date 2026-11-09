# The present API tests (docs/milestone-specs.md M6a, "Tests"; test names
# start with "present.<driver>."). Every test runs once per GPU driver,
# chosen through SE_GPU_DRIVER: direct3d12 and vulkan on Windows, vulkan
# elsewhere. A driver that cannot create a device skips the test with the
# reason.
#
#   [gpu]      a device without a window: the commit-gate presets run them
#              on Windows; the Linux presets exclude the label.
#   [desktop]  a window shown without taking focus (scripts/ci.ps1
#              -Desktop); never moves the mouse or sends keystrokes.
#
# The unpatched preset (msvc-unpatched-dev) builds the same tests against
# SE_SDL_PATCHED=0: every capability false, every action a no-op, frames
# still presented. PROPERTIES takes one value per property (catch's
# discovery would split a list value into further property names). The run
# name (--name) differs per driver because Catch.cmake names its generated
# files from the arguments, not from TEST_PREFIX.

se_add_module(se_present_tests KIND EXECUTABLE
  SOURCES
    present_test.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    se_core
    se_platform
    se_render
    Catch2::Catch2WithMain
  TEST)
target_include_directories(se_present_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")

if(WIN32)
  set(_se_present_drivers "d3d12=direct3d12" "vulkan=vulkan")
else()
  set(_se_present_drivers "vulkan=vulkan")
endif()
foreach(_se_entry IN LISTS _se_present_drivers)
  string(REPLACE "=" ";" _se_pair "${_se_entry}")
  list(GET _se_pair 0 _se_short)
  list(GET _se_pair 1 _se_driver)
  catch_discover_tests(se_present_tests
    TEST_SPEC "~[desktop]"
    EXTRA_ARGS "--name=present.${_se_short}"
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "present.${_se_short}."
    ADD_TAGS_AS_LABELS
    PROPERTIES
      ENVIRONMENT SE_GPU_DRIVER=${_se_driver}
      TIMEOUT 60)
  catch_discover_tests(se_present_tests
    TEST_SPEC "[desktop]"
    EXTRA_ARGS "--name=present.${_se_short}"
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "present.${_se_short}."
    ADD_TAGS_AS_LABELS
    PROPERTIES
      ENVIRONMENT SE_GPU_DRIVER=${_se_driver}
      TIMEOUT 60)
endforeach()
