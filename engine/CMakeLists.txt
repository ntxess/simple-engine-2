# Engine modules. Layering is enforced by se_check_link_graph() and the
# include scans (docs/decisions.md, ADR-004).

add_subdirectory(se_core)
add_subdirectory(se_os)
add_subdirectory(se_log)
add_subdirectory(se_sim)
add_subdirectory(se_render_core)
add_subdirectory(se_crash)

# The SDL3 platform, the renderer and the runtime (M5). The Linux presets
# build them too, with SDL's dummy and offscreen video only (no window-system
# packages); their window and GPU tests carry the desktop and gpu labels.
if(SE_BUILD_PLATFORM)
  add_subdirectory(se_platform)
  add_subdirectory(se_render)
  add_subdirectory(se_runtime)
endif()
