# game_rampage: the headless port of the legacy Sandbox's gameplay
# (docs/milestone-specs.md, M3b). Simulation code: DETERMINISTIC, no
# platform, GPU, audio or crash libraries. se_render_core (headless) only
# for SpriteRenderer, Camera2D and the sandbox's camera.

se_add_module(game_rampage KIND STATIC
  SOURCES
    src/broadphase.cc
    src/collision.cc
    src/effects.cc
    src/json.cc
    src/legacy_scenario.cc
    src/module.cc
    src/movement.cc
    src/projection.cc
    src/rampage_components.cc
    src/scene.cc
    src/stats.cc
    src/view.cc
  PUBLIC_DEPS
    se_core
    se_render_core
    se_sim
  FORBID
    SDL3::SDL3-static se_imgui miniaudio sentry lz4_static
  BANNED_INCLUDES
    ${SE_BAN_PLATFORM_INCLUDES}
  DETERMINISTIC)
