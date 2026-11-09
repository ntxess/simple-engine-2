# Renderer2D and GpuResources tests (docs/milestone-specs.md M7; test names
# start with "render_gpu." or "render_desktop."):
#
#   se_render_gpu_tests   Catch2. Untagged tests are label unit and run
#                         everywhere, the WSL presets included: the shader
#                         reflection, the texture table, PNGs, the image
#                         comparison and the frame-fault syntax. [gpu]
#                         tests need a device but no window (label gpu: the
#                         Windows commit gate runs them; excluded on Linux)
#                         and [desktop] tests open a window (label desktop:
#                         scripts/ci.ps1 -Desktop). Both are registered once
#                         per driver, with SE_GPU_DRIVER set; a driver the
#                         machine lacks skips them with the reason.
#
# Golden images are tests/golden/render/<driver>/<name>.png;
# SE_UPDATE_GOLDENS=1 rewrites them (scripts/ci.ps1 clears the variable).
# The rampage fault-injection tests (render.fault.*) are in
# tools/render_shot/CMakeLists.txt with the legacy image comparison.

set(_se_render_gpu_out "${CMAKE_BINARY_DIR}/render_gpu_out")
file(MAKE_DIRECTORY "${_se_render_gpu_out}")

# The fixture, scenes and image comparison, shared with tools/render_shot.
se_add_module(se_render_test_support KIND STATIC
  SOURCES
    support/gpu_fixture.cc
    support/image_compare.cc
  PUBLIC_DEPS
    se_core
    se_platform
    se_render
    se_render_core
  TEST)
target_include_directories(se_render_test_support PUBLIC
  "${CMAKE_SOURCE_DIR}/tests")
target_compile_definitions(se_render_test_support PRIVATE
  SE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  SE_RENDER_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/render"
  SE_TEST_OUT_DIR="${_se_render_gpu_out}")

se_add_module(se_render_gpu_tests KIND EXECUTABLE
  SOURCES
    desktop_test.cc
    gpu_resources_test.cc
    renderer_test.cc
    shader_reflection_test.cc
    texture_table_test.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    game_rampage
    rampage_test_support
    se_core
    se_platform
    se_platform_test_support
    se_render
    se_render_test_support
    se_runtime
    Catch2::Catch2WithMain
  TEST)

catch_discover_tests(se_render_gpu_tests
  TEST_SPEC "~[gpu]~[desktop]"
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "render_gpu."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS unit)

set(_se_gpu_drivers vulkan)
if(WIN32)
  list(PREPEND _se_gpu_drivers direct3d12)
endif()
# Catch.cmake names its discovery file from the test spec and not the
# prefix, so each driver's spec also excludes a tag no test has
# ([no-<driver>]).
foreach(_se_driver IN LISTS _se_gpu_drivers)
  if(_se_driver STREQUAL "direct3d12")
    set(_se_driver_dir d3d12)
  else()
    set(_se_driver_dir ${_se_driver})
  endif()
  catch_discover_tests(se_render_gpu_tests
    TEST_SPEC "[gpu]~[desktop]~[no-${_se_driver}]"
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "render_gpu.${_se_driver_dir}."
    ADD_TAGS_AS_LABELS
    PROPERTIES
      LABELS gpu
      ENVIRONMENT "SE_GPU_DRIVER=${_se_driver}"
      TIMEOUT 120)
  catch_discover_tests(se_render_gpu_tests
    TEST_SPEC "[desktop]~[no-${_se_driver}]"
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "render_desktop.${_se_driver_dir}."
    ADD_TAGS_AS_LABELS
    PROPERTIES
      LABELS desktop
      ENVIRONMENT "SE_GPU_DRIVER=${_se_driver}"
      TIMEOUT 120)
endforeach()
