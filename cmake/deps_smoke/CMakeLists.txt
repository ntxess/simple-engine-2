# deps-smoke preset: fetches and builds every pinned dependency once, to catch
# toolchain incompatibilities early, and times one glaze reflection TU
# (docs/deps.md records the result).

se_use_deps(catch2 spdlog glaze tracy miniaudio lz4 sdl3 imgui)
add_custom_target(se_deps_smoke)
add_dependencies(se_deps_smoke
  Catch2WithMain spdlog SDL3-static TracyClient miniaudio lz4_static)
if(SE_CRASH)
  se_use_deps(sentry)
  add_dependencies(se_deps_smoke sentry crashpad_handler)
endif()

add_executable(se_glaze_smoke glaze_smoke.cc)
target_link_libraries(se_glaze_smoke PRIVATE glaze::glaze)
add_dependencies(se_deps_smoke se_glaze_smoke)

if(NOT EXISTS "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp")
  message(FATAL_ERROR "imgui ${imgui_SOURCE_DIR} lacks imgui_impl_sdlgpu3")
endif()
