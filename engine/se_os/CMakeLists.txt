# se_os: the headless OS layer that threads need on every platform (ADR-029):
# StartThread and RoleScope (OS thread names, QoS, CPU sets), the CPU
# topology, the unbiased clock, debugger detection and the executable's
# directory. It may include <windows.h> and POSIX headers, but never SDL, so
# the SDL-free WSL sanitizer builds have it too. It depends on se_core alone
# (ALLOW_ONLY), and it is the only module that may create threads
# (tools/checks/check_banned_api.py).

se_add_module(se_os KIND STATIC
  SOURCES
    src/cpu_topology.cc
    src/debugger.cc
    src/process.cc
    src/thread.cc
    src/unbiased_clock.cc
  PUBLIC_DEPS
    se_core
  ALLOW_ONLY
    se_core
  FORBID
    SDL3::SDL3-static se_imgui miniaudio sentry lz4_static)
