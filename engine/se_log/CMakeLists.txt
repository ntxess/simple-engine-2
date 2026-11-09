# se_log: the log worker (ADR-032). It owns the spdlog sinks (a rotating
# file, stderr and the panel ring), used synchronously on its own thread
# only, and drains the per-thread LogRings the SE_LOG_* front end of se_core
# writes. It is the only module that includes spdlog
# (tools/checks/check_includes.py) and depends on se_core, se_os and spdlog
# alone (ALLOW_ONLY).

se_use_deps(spdlog)

se_add_module(se_log KIND STATIC
  SOURCES
    src/log_worker.cc
  PUBLIC_DEPS
    se_core
    se_os
  PRIVATE_DEPS
    spdlog::spdlog
  ALLOW_ONLY
    se_core
    se_os
    spdlog::spdlog
  FORBID
    SDL3::SDL3-static se_imgui miniaudio sentry lz4_static)
