// Copyright 2026 The simple-engine-2 Authors.
//
// The M5 bodies of the integration hooks (integration.h): M2 behaviour
// until the integration step wires se_os, se_log and se_crash.

#include "se_runtime/integration.h"

#include <SDL3/SDL_cpuinfo.h>

#include <cstdint>
#include <cstdio>
#include <expected>
#include <string_view>
#include <utility>

#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_runtime/app_config.h"

namespace se::integration {

// The role is a static token with nothing to lock (thread_role.cc), so the
// analysis is off for the two functions that take and drop it.
void EnterMainThread() noexcept SE_NO_THREAD_SAFETY_ANALYSIS {
  // INTEGRATION(M4a): RoleScope{kMain, "main", Qos::kHigh}.
  RegisterMainThread();
}

void LeaveMainThread() noexcept SE_NO_THREAD_SAFETY_ANALYSIS {
  // INTEGRATION(M4a): end the RoleScope.
  UnregisterMainThread();
}

std::expected<CrashCapture, Error> StartCrashCapture(
    const AppConfig& /*config*/) {
  // INTEGRATION(M4b): se::crash::Start(...). M5 runs without crash capture.
  CrashCapture capture;
  capture.active_ = false;
  return capture;
}

void RequestFaultDump(std::string_view /*reason*/) noexcept {
  // INTEGRATION(M4b): se::crash::DumpWithoutCrash(reason).
}

std::expected<Logging, Error> StartLogging(const AppConfig& config) {
  // INTEGRATION(M4a): se_log sinks. M5 logs synchronously to stderr.
  SetLogLevel(config.log_level);
  Logging logging;
  logging.active_ = true;
  return logging;
}

Logging::Logging(Logging&& other) noexcept
    : active_(std::exchange(other.active_, false)) {}

Logging& Logging::operator=(Logging&& other) noexcept {
  if (this != &other) active_ = std::exchange(other.active_, false);
  return *this;
}

Logging::~Logging() {
  if (!active_) return;
  // INTEGRATION(M4a): spdlog::shutdown() after the worker stopped.
  std::fflush(stderr);
}

std::expected<CpuInfo, Error> ReadCpuTopology() {
  // INTEGRATION(M4a): se::os::ReadCpuTopology().
  CpuInfo info;
  info.logical_cores = SDL_GetNumLogicalCPUCores();
  return info;
}

std::expected<HelperThreads, Error> StartHelperThreads(
    const AppConfig& /*config*/, const WatchdogInputs& /*inputs*/) {
  // INTEGRATION(M4a): LogWorker::Start. INTEGRATION(M4b + M12): Watchdog.
  HelperThreads threads;
  threads.running_ = true;
  return threads;
}

HelperThreads::HelperThreads(HelperThreads&& other) noexcept
    : running_(std::exchange(other.running_, false)) {}

HelperThreads& HelperThreads::operator=(HelperThreads&& other) noexcept {
  if (this != &other) {
    Stop();
    running_ = std::exchange(other.running_, false);
  }
  return *this;
}

HelperThreads::~HelperThreads() { Stop(); }

void HelperThreads::Stop() noexcept {
  if (!running_) return;
  // INTEGRATION(M4a): RouteSdlLogToDefault(); log_worker.Stop().
  // INTEGRATION(M4b + M12): watchdog.RequestStop(); join (last).
  running_ = false;
}

void HelperThreads::ArmWatchdog() noexcept {
  // INTEGRATION(M4b + M12): watchdog.Arm().
}

void StartShutdownDeadline() noexcept {
  // INTEGRATION(M4b + M12): watchdog.StartShutdownDeadline().
}

std::uint32_t EngineThreadsRunning() noexcept {
  // INTEGRATION(M4a): ThreadRegistry live slots other than main. M5 starts
  // no engine threads.
  return 0;
}

void ShutdownLoggingOnly() noexcept {
  // INTEGRATION(M4a): stop the log worker if it started; spdlog::shutdown().
  std::fflush(stderr);
}

}  // namespace se::integration
