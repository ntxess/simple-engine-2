# se_core: the headless kernel. std, EnTT and glaze, plus Tracy in SE_TRACY
# builds. glaze is public because game modules' *_components.cc include
# reflect_glaze.h. Tracy is private: only src/profile.cc includes it, and it
# is built so that nothing starts before profile::Start() (ADR-029).

se_use_deps(glaze)
if(SE_TRACY)
  se_use_deps(tracy)
  # TRACY_DELAYED_INIT has no CMake option. TRACY_DBGHELP_LOCK names the
  # lock Tracy takes around its own DbgHelp calls (src/profile.cc).
  target_compile_definitions(TracyClient PUBLIC TRACY_DELAYED_INIT)
  if(WIN32)
    target_compile_definitions(TracyClient PUBLIC
      TRACY_DBGHELP_LOCK=SeDbgHelp)
  endif()
endif()

se_add_module(se_core KIND STATIC
  SOURCES
    src/assert.cc
    src/breadcrumb.cc
    src/component_registry.cc
    src/core_components.cc
    src/detmath.cc
    src/dump_stream.cc
    src/edge_placement.cc
    src/fork_fault_boundary.cc
    src/fp.cc
    src/guid.cc
    src/hash.cc
    src/input_buffer.cc
    src/input_frame_ring.cc
    src/jobs.cc
    src/log.cc
    src/log_ring.cc
    src/profile.cc
    src/profile_clock.cc
    src/schedule.cc
    src/sim_clock.cc
    src/snapshot_channel.cc
    src/thread_registry.cc
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
# Internal headers are included as "src/<name>.h" (the threaded pool's fault
# boundary, M14).
target_include_directories(se_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(SE_TRACY)
  target_link_libraries(se_core PRIVATE TracyClient)
  target_compile_definitions(se_core PUBLIC SE_TRACY=1)
endif()
