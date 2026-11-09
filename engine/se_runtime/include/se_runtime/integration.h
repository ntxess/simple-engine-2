// Copyright 2026 The simple-engine-2 Authors.
//
// The integration hooks: where the M4 parts (se_os, se_log, se_crash) plug
// into the runtime (docs/milestone-specs.md, "Integration (M4a + M4b +
// M5)"). The M5 track built the runtime without them; every function here
// is the single place the integration step fills in, and each says what
// goes there. Until then they keep M2's behaviour (SetCurrentRole,
// RegisterMainThread, synchronous logging) and do nothing else.
//
// Boot order (App::Create; mt-design.md "Booting -> Running"):
//   1 crash-handler   StartCrashCapture          (first boot step)
//   2 logging         StartLogging               (sinks; no threads yet)
//   3 sdl .. 7 gpu-device                        (se_platform, se_render)
//   8 cpu-topology    ReadCpuTopology
//   9 helper-threads  StartHelperThreads         (log worker, watchdog)
//  10 game
// Shutdown (App::Teardown; mt-design.md "Shutdown Order"):
//   StartShutdownDeadline when the quit drain ends (step 3), ..., then
//   StopHelperThreads (16 log worker, 17 watchdog join) after
//   SDL_QuitSubSystem, then the logging and crash scopes end.

#ifndef SE_RUNTIME_INTEGRATION_H_
#define SE_RUNTIME_INTEGRATION_H_

#include <atomic>
#include <cstdint>
#include <expected>
#include <string_view>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_runtime/app_config.h"

namespace se::integration {

// The main thread's identity: EnterMainThread is the first thing
// SDL_AppInit does and LeaveMainThread the last thing SDL_AppQuit does.
// INTEGRATION(M4a): construct se::RoleScope{ThreadRole::kMain, "main",
// Qos::kHigh} (thread name, ThreadRegistry slot 0, HighQoS, the main
// thread's log ring) and keep it until LeaveMainThread. M5 calls
// RegisterMainThread (which sets the role) and UnregisterMainThread. The
// callbacks then take g_main for the analysis with AssertMainThread().
void EnterMainThread() noexcept;
void LeaveMainThread() noexcept;

// Boot step 1, before logging and SDL.
// INTEGRATION(M4b): se::crash::Start(CrashConfig{database under the pref
// path or the executable directory, crashpad_handler next to the
// executable, annotations}); keep the CrashGuard here. Failure is not
// fatal: log it and run without crash capture (return a disabled scope).
class CrashCapture {
 public:
  [[nodiscard]] bool Active() const noexcept { return active_; }

 private:
  friend std::expected<CrashCapture, Error> StartCrashCapture(
      const AppConfig& config);
  bool active_ = false;
};
[[nodiscard]] std::expected<CrashCapture, Error> StartCrashCapture(
    const AppConfig& config);

// A fault in an SDL callback asks for an out-of-process dump here.
// INTEGRATION(M4b): se::crash::DumpWithoutCrash(reason).
void RequestFaultDump(std::string_view reason) noexcept;

// Boot step 2: the log sinks, before any other engine thread exists.
// INTEGRATION(M4a): configure se_log's sinks (file under the pref path,
// stderr at config.log_level, the panel ring); the worker itself starts in
// StartHelperThreads.
class Logging {
 public:
  Logging() = default;
  Logging(Logging&& other) noexcept;
  Logging& operator=(Logging&& other) noexcept;
  Logging(const Logging&) = delete;
  Logging& operator=(const Logging&) = delete;
  ~Logging();

 private:
  friend std::expected<Logging, Error> StartLogging(const AppConfig& config);
  bool active_ = false;
};
[[nodiscard]] std::expected<Logging, Error> StartLogging(
    const AppConfig& config);

// What the watchdog reads (atomics only; docs/design/mt-design.md "Data
// Flow": iterate counter, Suspended, Modal and heartbeat flags).
struct WatchdogInputs {
  const std::atomic<std::uint64_t>* iterate_counter = nullptr;
  const std::atomic<bool>* modal = nullptr;
  const std::atomic<bool>* suspended = nullptr;
  const std::atomic<bool>* heartbeat = nullptr;
};

// Boot step 8.
// INTEGRATION(M4a): se::os::ReadCpuTopology(); size the (still empty)
// ForkJoinPool from ForkPoolSize(topology).
struct CpuInfo {
  int logical_cores = 0;
};
[[nodiscard]] std::expected<CpuInfo, Error> ReadCpuTopology();

// Boot step 9: the helper threads.
// INTEGRATION(M4a): LogWorker::Start (role kLogWorker).
// INTEGRATION(M4b + M12): the Watchdog skeleton (role kWatchdog, boot
// budget 30 s, armed after the first iterate) reading |inputs|.
// M9/M8/M6 add the lane, input sampler and vblank sampler here.
class HelperThreads {
 public:
  HelperThreads() = default;
  HelperThreads(HelperThreads&& other) noexcept;
  HelperThreads& operator=(HelperThreads&& other) noexcept;
  HelperThreads(const HelperThreads&) = delete;
  HelperThreads& operator=(const HelperThreads&) = delete;
  ~HelperThreads();

  // Shutdown steps 16 and 17: stop and drain the log worker (pointing
  // SDL's log output at stderr first), then join the watchdog, the last
  // engine thread. Idempotent.
  void Stop() noexcept;
  // The first iterate ran: the watchdog leaves its boot budget.
  void ArmWatchdog() noexcept;

 private:
  friend std::expected<HelperThreads, Error> StartHelperThreads(
      const AppConfig& config, const WatchdogInputs& inputs);
  bool running_ = false;
};
[[nodiscard]] std::expected<HelperThreads, Error> StartHelperThreads(
    const AppConfig& config, const WatchdogInputs& inputs);

// Shutdown step 3: the quit drain is over; the watchdog's 5 s deadline
// starts.
// INTEGRATION(M4b + M12): Watchdog::StartShutdownDeadline().
void StartShutdownDeadline() noexcept;

// Engine threads still running (the init-failure tests require 0).
// INTEGRATION(M4a): the ThreadRegistry's live slots other than main.
[[nodiscard]] std::uint32_t EngineThreadsRunning() noexcept;

// SDL_AppQuit after a failed init: only logging shuts down.
// INTEGRATION(M4a): stop the log worker if it started, spdlog::shutdown().
void ShutdownLoggingOnly() noexcept;

}  // namespace se::integration

#endif  // SE_RUNTIME_INTEGRATION_H_
