# se_sim: the headless runtime half (GameModule, GameStateStack, SessionLog,
# replays, SimDriver), and the dedicated sim driver's clock and input folder
# (M17a: PacedClock, InputFolder; ADR-084) and its thread (SimThread, with
# the sim's tick as a fault boundary, sim_fault_boundary.cc; ADR-083). No
# SDL, so tests and WSL sanitizer builds stay headless (ADR-004). se_os is
# private: only system_wait_clock.cc (the precise timer) and sim_thread.cc
# (StartThread) include it, so game modules never see an OS header.

se_add_module(se_sim KIND STATIC
  SOURCES
    src/beve.cc
    src/bytes.cc
    src/content_source.cc
    src/game_module.cc
    src/game_state.cc
    src/input_folder.cc
    src/paced_clock.cc
    src/replay.cc
    src/session_log.cc
    src/sim_driver.cc
    src/sim_fault_boundary.cc
    src/sim_thread.cc
    src/synthetic_clock.cc
    src/system_wait_clock.cc
  PUBLIC_DEPS
    se_core
  PRIVATE_DEPS
    se_os
  FORBID
    SDL3::SDL3-static se_imgui miniaudio sentry lz4_static
  BANNED_INCLUDES
    ${SE_BAN_PLATFORM_INCLUDES}
  DETERMINISTIC)

# Internal headers are included as "src/<name>.h".
target_include_directories(se_sim PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(se_sim PRIVATE
  "SE_BUILD_NAME=\"simple-engine ${PROJECT_VERSION} $<CONFIG>\"")
