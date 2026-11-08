# se_sim: the headless runtime half (GameModule, GameStateStack, SessionLog,
# replays, SimDriver). No SDL, so tests and WSL sanitizer builds stay
# headless (ADR-004).

se_add_module(se_sim KIND STATIC
  SOURCES
    src/beve.cc
    src/bytes.cc
    src/game_module.cc
    src/game_state.cc
    src/replay.cc
    src/session_log.cc
    src/sim_driver.cc
    src/synthetic_clock.cc
  PUBLIC_DEPS
    se_core
  FORBID
    SDL3::SDL3-static se_imgui miniaudio sentry lz4_static
  BANNED_INCLUDES
    ${SE_BAN_PLATFORM_INCLUDES}
  DETERMINISTIC)

target_compile_definitions(se_sim PRIVATE
  "SE_BUILD_NAME=\"simple-engine ${PROJECT_VERSION} $<CONFIG>\"")
