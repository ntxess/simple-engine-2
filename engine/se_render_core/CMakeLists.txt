# se_render_core: the headless half of the renderer (ADR-004): the sprite and
# camera components, the RenderList builder and the sprite vertices (M7). No
# SDL, GPU or ImGui, so the tests, se_bench and the WSL sanitizer builds stay
# headless. It reads Worlds through const references only and may depend on
# se_core (and EnTT) alone (ALLOW_ONLY, checked by se_check_link_graph).

se_add_module(se_render_core KIND STATIC
  SOURCES
    src/render_components.cc
    src/render_list.cc
    src/sprite_vertices.cc
  PUBLIC_DEPS
    se_core
  ALLOW_ONLY
    se_core
  FORBID
    SDL3::SDL3-static se_imgui miniaudio sentry lz4_static
  BANNED_INCLUDES
    ${SE_BAN_PLATFORM_INCLUDES}
  DETERMINISTIC)
