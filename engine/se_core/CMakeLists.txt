# se_core: the headless kernel. std, EnTT, glaze and spdlog only. glaze is
# public because game modules' *_components.cc include reflect_glaze.h.

se_use_deps(glaze)

se_add_module(se_core KIND STATIC
  SOURCES
    src/assert.cc
    src/breadcrumb.cc
    src/component_registry.cc
    src/core_components.cc
    src/detmath.cc
    src/fp.cc
    src/guid.cc
    src/hash.cc
    src/jobs.cc
    src/log.cc
    src/profile_clock.cc
    src/sim_clock.cc
    src/thread_role.cc
    src/time.cc
    src/world.cc
  PUBLIC_DEPS
    se::entt
    glaze::glaze
  FORBID
    SDL3::SDL3-static se_imgui miniaudio sentry lz4_static
  BANNED_INCLUDES
    ${SE_BAN_PLATFORM_INCLUDES}
  DETERMINISTIC)

target_compile_definitions(se_core PUBLIC
  $<IF:$<CONFIG:Release>,SE_ASSERTS=0,SE_ASSERTS=1>)
