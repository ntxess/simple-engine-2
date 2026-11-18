# Engine modules. Layering is enforced by se_check_link_graph() and the
# include scans (docs/decisions.md, ADR-004).

add_subdirectory(se_core)
add_subdirectory(se_os)
add_subdirectory(se_log)
add_subdirectory(se_sim)
add_subdirectory(se_render_core)
add_subdirectory(se_crash)
# The audio engine on miniaudio (M8): main-only, se_core only.
add_subdirectory(se_audio)

# The SDL3 platform, the renderer and the runtime (M5). The Linux presets
# build them too, with SDL's dummy and offscreen video only (no window-system
# packages); their window and GPU tests carry the desktop and gpu labels.
if(SE_BUILD_PLATFORM)
  add_subdirectory(se_platform)
  # M9: the background lane and the AssetRegistry (se::fs is se_platform's).
  add_subdirectory(se_assets)
  add_subdirectory(se_render)
  add_subdirectory(se_runtime)
  # M10a: Dear ImGui and the editor shell (the game build never links them).
  add_subdirectory(se_imgui)
  add_subdirectory(se_editor)
  # M9: the texture cook, run at build time into <build>/bin/cooked.
  add_subdirectory("${CMAKE_SOURCE_DIR}/tools/asset_cook"
                   "${CMAKE_BINARY_DIR}/tools/asset_cook")
  # M8: the key stamp probe (needs a person; docs/gates.md).
  add_subdirectory("${CMAKE_SOURCE_DIR}/tools/input_probe"
                   "${CMAKE_BINARY_DIR}/tools/input_probe")
  # M10b: the editor's panels (over se_editor and game_rampage).
  add_subdirectory(se_editor/panels)
endif()
