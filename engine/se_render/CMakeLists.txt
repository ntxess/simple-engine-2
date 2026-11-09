# se_render: the SDL_GPU device (M5) and Renderer2D on main (M7): the frame
# transaction, GpuResources (handles, the retire queue, the upload queue),
# the sprite and blit pipelines, PNG decoding with stb_image and the texture
# table (docs/milestone-specs.md M5 and M7). Main thread only; not
# deterministic. Draws se_render_core's RenderLists.
# M6a: present.h, the presentation signals of the SDL patch (SE_SDL_PATCHED
# is 1 when cmake/Dependencies.cmake applied it, else every query reports
# unsupported).

se_use_deps(stb)

se_add_module(se_render KIND STATIC
  SOURCES
    src/frame_faults.cc
    src/frame_recorder.cc
    src/gpu_device.cc
    src/gpu_resources.cc
    src/image_decode.cc
    src/png_write.cc
    src/present.cc
    src/renderer_2d.cc
    src/shaders.cc
    src/texture_table.cc
  PUBLIC_DEPS
    se_core
    se_platform
    se_render_core
    SDL3::SDL3-static
  PRIVATE_DEPS
    se::stb
  FORBID
    se_imgui miniaudio sentry lz4_static)
if(SE_SDL_PATCHED)
  target_compile_definitions(se_render PUBLIC SE_SDL_PATCHED=1)
else()
  target_compile_definitions(se_render PUBLIC SE_SDL_PATCHED=0)
endif()

# Internal headers are included as "src/<name>.h", the committed SPIR-V
# headers as "shaders/generated/<name>.spv.h".
target_include_directories(se_render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Shaders (ADR-059). HLSL in shaders/. SPIR-V is committed under
# shaders/generated/ by tools/shaders/build_shaders.py (glslang in WSL);
# the freshness test regenerates it in the WSL presets. DXIL is compiled
# here at build time with the Windows SDK's dxc.exe.
set(_se_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(_se_shader_tool "${CMAKE_SOURCE_DIR}/tools/shaders/build_shaders.py")
set(_se_shaders sprite.vert sprite.frag blit.vert blit.frag)
if(WIN32)
  # The MSVC environment names the SDK's bin directory; otherwise the SDK
  # root, at the version the engine was checked with.
  set(_se_dxc_hints)
  if(DEFINED ENV{WindowsSdkVerBinPath})
    list(APPEND _se_dxc_hints "$ENV{WindowsSdkVerBinPath}/x64")
  endif()
  if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
    string(REGEX REPLACE "[\\/]$" "" _se_sdk_version
           "$ENV{WindowsSDKVersion}")
    list(APPEND _se_dxc_hints
      "$ENV{WindowsSdkDir}/bin/${_se_sdk_version}/x64")
  endif()
  list(APPEND _se_dxc_hints
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64")
  find_program(SE_DXC dxc HINTS ${_se_dxc_hints} NO_DEFAULT_PATH)
  if(NOT SE_DXC)
    message(FATAL_ERROR
      "se_render: dxc.exe from the Windows SDK (10.0.26100.0, x64) was not "
      "found; it compiles the renderer's HLSL to DXIL. Install the Windows "
      "SDK or set SE_DXC to its dxc.exe (searched: ${_se_dxc_hints}).")
  endif()
  set(_se_dxil_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  set(_se_dxil_headers)
  foreach(_se_shader IN LISTS _se_shaders)
    set(_se_source "${_se_shader_dir}/${_se_shader}.hlsl")
    set(_se_header "${_se_dxil_dir}/${_se_shader}.dxil.h")
    add_custom_command(
      OUTPUT "${_se_header}"
      COMMAND Python3::Interpreter "${_se_shader_tool}" dxil
              --dxc "${SE_DXC}" --source "${_se_source}" --out "${_se_header}"
      DEPENDS "${_se_source}" "${_se_shader_tool}"
      COMMENT "dxc ${_se_shader}.hlsl"
      VERBATIM)
    list(APPEND _se_dxil_headers "${_se_header}")
  endforeach()
  add_custom_target(se_render_dxil DEPENDS ${_se_dxil_headers})
  add_dependencies(se_render se_render_dxil)
  # Included as "shaders/<name>.dxil.h".
  target_include_directories(se_render PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_compile_definitions(se_render PUBLIC SE_RENDER_HAS_DXIL=1)
endif()

# The SPIR-V freshness check (label checks) runs where glslang runs
# natively: the WSL presets. Its planted twin holds one stale header, which
# the check must report.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND SE_BUILD_TESTS)
  add_test(NAME checks.shaders.spirv_fresh
    COMMAND Python3::Interpreter "${_se_shader_tool}" spirv --check)
  add_test(NAME checks.planted.spirv_stale
    COMMAND Python3::Interpreter "${_se_shader_tool}" spirv --check
            --expect-stale
            --generated-dir "${CMAKE_SOURCE_DIR}/tools/shaders/planted/stale")
  set_tests_properties(checks.shaders.spirv_fresh checks.planted.spirv_stale
    PROPERTIES LABELS checks)
endif()
