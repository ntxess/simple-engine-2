# render_shot: a rampage scenario at one tick rendered by se_render on a GPU
# device without a window, and the PNG comparison the legacy and fault
# tests use (docs/milestone-specs.md M7). Tool code (TEST: exempt from the
# banned-API scan), built with the tests because it links their support
# libraries.
#
#   render.legacy_image.<driver>   labels gpu and legacy: legacy_replay
#                                  --image-out 600 and render_shot of the
#                                  stress scenario at tick 600 match within
#                                  the golden tolerance. Registered when a
#                                  legacy build of this checkout exists
#                                  (out/build/legacy-msvc-release or -debug)
#                                  or SE_LEGACY_REPLAY names legacy_replay.
#   render.fault.<driver>.<step>   label desktop: the rampage executable
#                                  with --inject-frame-fault STEP:1:100 for
#                                  frames 1-100; build and vertices quit
#                                  through the fault path, every other step
#                                  keeps running, and its frame-140
#                                  readback matches a run without faults.
#                                  Both leak no GPU object.
#   tools.compare_images           compare_images.py on synthetic PNGs.

se_add_module(render_shot KIND EXECUTABLE
  SOURCES
    main.cc
  PRIVATE_DEPS
    game_rampage
    rampage_test_support
    se_core
    se_platform
    se_render
    se_render_test_support
  TEST)

set(_se_shot_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(_se_shot_out "${CMAKE_BINARY_DIR}/render_shot_out")
file(MAKE_DIRECTORY "${_se_shot_out}")

add_test(NAME tools.compare_images
  COMMAND Python3::Interpreter "${_se_shot_dir}/compare_images_test.py")
set_tests_properties(tools.compare_images PROPERTIES LABELS "unit;tools")

set(_se_gpu_drivers vulkan)
if(WIN32)
  list(PREPEND _se_gpu_drivers direct3d12)
endif()

# The legacy image comparison, when the legacy reference is built.
set(SE_LEGACY_REPLAY "" CACHE FILEPATH
    "legacy_replay for the legacy image comparison (default: a legacy build tree of this checkout)")
set(_se_legacy_replay "${SE_LEGACY_REPLAY}")
if(NOT _se_legacy_replay)
  foreach(_se_tree legacy-msvc-release legacy-msvc-debug)
    set(_se_candidate
        "${CMAKE_SOURCE_DIR}/out/build/${_se_tree}/app/legacy_replay${CMAKE_EXECUTABLE_SUFFIX}")
    if(EXISTS "${_se_candidate}")
      set(_se_legacy_replay "${_se_candidate}")
      break()
    endif()
  endforeach()
endif()
if(_se_legacy_replay)
  foreach(_se_driver IN LISTS _se_gpu_drivers)
    set(_se_dir ${_se_driver})
    if(_se_driver STREQUAL "direct3d12")
      set(_se_dir d3d12)
    endif()
    add_test(NAME render.legacy_image.${_se_dir}
      COMMAND Python3::Interpreter "${_se_shot_dir}/legacy_image_test.py"
              --legacy-replay "${_se_legacy_replay}"
              --render-shot "$<TARGET_FILE:render_shot>"
              --driver ${_se_driver}
              --scenario stress --tick 600
              --work "${_se_shot_out}/legacy_${_se_dir}"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(render.legacy_image.${_se_dir} PROPERTIES
      LABELS "gpu;legacy" TIMEOUT 120 SKIP_RETURN_CODE 77)
  endforeach()
else()
  message(STATUS "render_shot: no legacy_replay found; the legacy image "
                 "comparison is not registered (build legacy-msvc-release "
                 "or set SE_LEGACY_REPLAY)")
endif()

# Fault injection at every step of the frame transaction, in the rampage
# executable (desktop: a real window, shown without focus).
if(TARGET rampage AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  foreach(_se_driver IN LISTS _se_gpu_drivers)
    set(_se_dir ${_se_driver})
    if(_se_driver STREQUAL "direct3d12")
      set(_se_dir d3d12)
    endif()
    set(_se_work "${_se_shot_out}/fault_${_se_dir}")
    set(_se_fault_args
      Python3::Interpreter "${_se_shot_dir}/fault_test.py"
      --rampage "$<TARGET_FILE:rampage>"
      --render-shot "$<TARGET_FILE:render_shot>"
      --driver ${_se_driver} --work "${_se_work}")
    add_test(NAME render.fault.${_se_dir}.reference
      COMMAND ${_se_fault_args} --step none)
    set_tests_properties(render.fault.${_se_dir}.reference PROPERTIES
      LABELS desktop TIMEOUT 120 SKIP_RETURN_CODE 77
      FIXTURES_SETUP render_fault_reference_${_se_dir})
    # device_lost: --inject-device-lost (the M6/M7 integration, ADR-068).
    foreach(_se_step build vertices upload prepare copy_pass viewport_pass
            acquire swapchain_pass submit device_lost)
      add_test(NAME render.fault.${_se_dir}.${_se_step}
        COMMAND ${_se_fault_args} --step ${_se_step})
      set_tests_properties(render.fault.${_se_dir}.${_se_step} PROPERTIES
        LABELS desktop TIMEOUT 120 SKIP_RETURN_CODE 77
        FIXTURES_REQUIRED render_fault_reference_${_se_dir})
    endforeach()
  endforeach()
endif()
