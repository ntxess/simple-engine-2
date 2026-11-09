# se_crash: out-of-process crash capture (docs/milestone-specs.md, M4b;
# ADR-033). On Windows it uses the crashpad client from the pinned
# sentry-native archive (SENTRY_BACKEND=crashpad, SENTRY_TRANSPORT=none; the
# sentry library itself is never linked). Other builds get the stub backend,
# with the same API: SE_CRASH=OFF (msvc-asan, ADR-034) and Linux, where
# crashpad's build needs libcurl development files that the WSL machine does
# not have (docs/gates.md, "Linux crash capture").
#
# se_crash depends on se_core and crashpad only; the integration adds se_os.

set(SE_CRASH_BACKEND stub)
if(SE_CRASH AND WIN32)
  se_use_deps(sentry)
  set(SE_CRASH_BACKEND crashpad)
elseif(SE_CRASH)
  message(STATUS "se_crash: the crashpad backend is Windows-only for now "
                 "(docs/gates.md, \"Linux crash capture\"); using the stub")
endif()
# tests/crash registers the crash tests only for the crashpad backend.
set_property(GLOBAL PROPERTY SE_CRASH_BACKEND "${SE_CRASH_BACKEND}")

if(SE_CRASH_BACKEND STREQUAL "crashpad")
  set(_se_crash_sources src/crash_crashpad_win.cc src/terminate_handler.cc)
  set(_se_crash_deps crashpad::client)
else()
  set(_se_crash_sources src/crash_stub.cc)
  set(_se_crash_deps "")
endif()

se_add_module(se_crash KIND STATIC
  SOURCES
    src/stream_table.cc
    ${_se_crash_sources}
  PUBLIC_DEPS
    se_core
  PRIVATE_DEPS
    ${_se_crash_deps}
  ALLOW_ONLY
    se_core crashpad_client
  FORBID
    SDL3::SDL3-static se_imgui miniaudio sentry lz4_static
  BANNED_INCLUDES
    "^SDL3?/" "^imgui" "^backends/imgui" "^miniaudio" "^sentry" "^lz4"
    "^SFML/" "^boost/" "^[Dd]bg[Hh]elp\\.h$")
# Internal headers are included as "src/<name>.h".
target_include_directories(se_crash PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(se_crash PUBLIC
  $<IF:$<STREQUAL:${SE_CRASH_BACKEND},crashpad>,SE_CRASH_CRASHPAD=1,SE_CRASH_CRASHPAD=0>)

# se_crash_copy_handler(<target>): copies crashpad_handler next to the
# executable <target> (its RUNTIME_OUTPUT_DIRECTORY), before <target> is
# built and again whenever the handler is rebuilt, so that
# se::crash::HandlerPathIn(ExecutableDir()) finds it. Every executable that
# starts crash capture calls it; it does nothing in stub builds.
function(se_crash_copy_handler target)
  if(NOT TARGET crashpad_handler)
    return()
  endif()
  get_target_property(_dir ${target} RUNTIME_OUTPUT_DIRECTORY)
  if(NOT _dir)
    message(FATAL_ERROR "se_crash_copy_handler(${target}): the target has no "
                        "RUNTIME_OUTPUT_DIRECTORY (use se_add_module)")
  endif()
  # One copy rule per output directory: a file has one rule that makes it.
  string(MD5 _id "${_dir}")
  set(_copy "se_crash_handler_${_id}")
  set(_out "${_dir}/crashpad_handler${CMAKE_EXECUTABLE_SUFFIX}")
  if(NOT TARGET ${_copy})
    add_custom_command(OUTPUT "${_out}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "$<TARGET_FILE:crashpad_handler>" "${_out}"
      DEPENDS crashpad_handler
      COMMENT "Copying crashpad_handler to ${_dir}"
      VERBATIM)
    add_custom_target(${_copy} DEPENDS "${_out}")
  endif()
  add_dependencies(${target} ${_copy})
endfunction()
