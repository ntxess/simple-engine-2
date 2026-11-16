# se_imgui: Dear ImGui v1.93.0-docking for the editor (docs/milestone-specs.md
# M10a; ADR-095). The only target that compiles ImGui: its sources, the
# first-party SDL3 platform and SDL_GPU renderer backends from the release
# archive and misc/cpp/imgui_stdlib, built as they are (third-party: not
# linted, built without the engine's warning set), with
# se_imgui/imconfig_se.h as IMGUI_USER_CONFIG everywhere: a thread_local
# GImGui and IM_ASSERT on in every build. se_imgui's own code (the context,
# the headless frames, the renderer integration with M7's Renderer2D) is
# engine code. Main thread only; the game build never links it (rampage,
# se_runtime and se_render FORBID it).

se_use_deps(imgui)

set(_se_imgui_dir "${imgui_SOURCE_DIR}")
if(NOT EXISTS "${_se_imgui_dir}/backends/imgui_impl_sdlgpu3.cpp")
  message(FATAL_ERROR "se_imgui: ${_se_imgui_dir} lacks the SDL_GPU backend")
endif()

# ImGui as it ships, into se_imgui's own objects.
add_library(se_imgui_third_party OBJECT
  "${_se_imgui_dir}/imgui.cpp"
  "${_se_imgui_dir}/imgui_demo.cpp"
  "${_se_imgui_dir}/imgui_draw.cpp"
  "${_se_imgui_dir}/imgui_tables.cpp"
  "${_se_imgui_dir}/imgui_widgets.cpp"
  "${_se_imgui_dir}/misc/cpp/imgui_stdlib.cpp"
  "${_se_imgui_dir}/backends/imgui_impl_sdl3.cpp"
  "${_se_imgui_dir}/backends/imgui_impl_sdlgpu3.cpp")
target_include_directories(se_imgui_third_party PUBLIC
  "${_se_imgui_dir}"
  "${_se_imgui_dir}/backends"
  "${_se_imgui_dir}/misc/cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(se_imgui_third_party PUBLIC
  "IMGUI_USER_CONFIG=\"se_imgui/imconfig_se.h\"")
# IM_ASSERT calls se::AssertFailed (se_core/assert.h).
target_link_libraries(se_imgui_third_party PUBLIC se_core SDL3::SDL3-static)
if(MSVC)
  target_compile_options(se_imgui_third_party PRIVATE /W3 /utf-8)
endif()

se_add_module(se_imgui KIND STATIC
  SOURCES
    src/imgui_context.cc
    $<TARGET_OBJECTS:se_imgui_third_party>
  PUBLIC_DEPS
    se_core
    se_render
    SDL3::SDL3-static
  FORBID
    miniaudio sentry lz4_static)
# Engine code includes ImGui's headers as third-party ones (<imgui.h>,
# <imgui_impl_sdl3.h>, <imgui_stdlib.h>): SYSTEM, so their warnings stay
# out of engine builds.
target_include_directories(se_imgui SYSTEM PUBLIC
  "${_se_imgui_dir}"
  "${_se_imgui_dir}/backends"
  "${_se_imgui_dir}/misc/cpp")
target_compile_definitions(se_imgui PUBLIC
  "IMGUI_USER_CONFIG=\"se_imgui/imconfig_se.h\"")
