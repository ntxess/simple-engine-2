// Copyright 2026 The simple-engine-2 Authors.
//
// Where the M4 parts (se_os, se_log, se_crash) plug into the runtime
// (docs/milestone-specs.md, "Integration (M4a + M4b + M5)"; ADR-037,
// ADR-042). Main thread only, unless a function says otherwise.
//
// Boot order (App::Create; mt-design.md "Booting -> Running"):
//   -  main thread     EnterMainThread: RoleScope{kMain, "main", HighQoS}
//                      (ThreadRegistry slot 0 and its 4096-record ring)
//   1 crash-handler   StartCrashCapture: the dump streams, then crashpad's
//                      out-of-process handler
//   2 logging         StartLogging: the log worker, with its sinks, before
//                      any other engine thread
//   3 sdl .. 7 gpu-device                        (se_platform, se_render)
//   8 cpu-topology    ReadCpuTopology
//   9 helper-threads  StartHelperThreads: the vblank sampler (Windows, a
//                      window), then the watchdog (the boot budget until the
//                      first iterate arms it; hangs from then on, M12)
//  10 game            the InputFrame ring attached to the SimDriver; the
//                      session header begun (M12)
// Shutdown (App::Teardown; mt-design.md "Shutdown Order"):
//   3 HelperThreads::StartShutdownDeadline when the quit drain ends; ...;
//   9 HelperThreads::StopSamplers: the vblank sampler (joined within a
//     refresh plus 50 ms, or detached while stuck in its wait); ...;
//  16 Logging::Stop (SDL's log to stderr, drain, spdlog::shutdown) after
//     SDL_QuitSubSystem; 17 HelperThreads::Stop joins the watchdog, the
//     last engine thread. Crash capture lasts until the process exits.

#ifndef SE_RUNTIME_INTEGRATION_H_
#define SE_RUNTIME_INTEGRATION_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"
#include "se_core/input_frame_ring.h"
#include "se_core/session_header.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_crash/watchdog.h"
#include "se_log/log_worker.h"
#include "se_platform/display_output.h"
#include "se_platform/vblank_sampler.h"
#include "se_runtime/app_config.h"

namespace se {
class LogWorker;
}  // namespace se

namespace se::integration {

// The main thread's identity, from the first thing SDL_AppInit does to the
// last thing SDL_AppQuit does: RegisterMainThread and a RoleScope{kMain,
// "main", Qos::kHigh} (the OS thread name, ThreadRegistry slot 0 with the
// main thread's log ring, HighQoS). The callbacks then take g_main for the
// analysis with AssertMainThread().
void EnterMainThread() noexcept;
void LeaveMainThread() noexcept;

// Boot step 1, before logging and SDL. The first call in a process
// registers the dump streams (the ThreadRegistry's breadcrumb table, every
// log ring as its slot is allocated, the library ring and the InputFrame
// ring) and starts crashpad's handler, crashpad_handler next to the
// executable, with its database in |database_dir|, unless |start_handler|
// is false (--no-crash-capture). Crash capture then lasts until the
// process exits (crashpad cannot be stopped or started twice), so later
// calls report the same state. Failure is never fatal: the scope is
// inactive and Status() says why, for the log once logging is up.
class CrashCapture {
 public:
  [[nodiscard]] bool Active() const noexcept { return active_; }
  [[nodiscard]] const std::string& Status() const noexcept { return status_; }

 private:
  friend std::expected<CrashCapture, Error> StartCrashCapture(
      std::string_view database_dir, std::string_view app_name,
      bool start_handler);
  bool active_ = false;
  std::string status_;
};
[[nodiscard]] std::expected<CrashCapture, Error> StartCrashCapture(
    std::string_view database_dir, std::string_view app_name,
    bool start_handler);

// A fault in an SDL callback, or one caught in SDL_AppInit, asks the
// handler for a dump of the process as it is (DumpWithoutCrash on the
// faulting thread). Does nothing without crash capture.
void RequestFaultDump(std::string_view reason) noexcept;

// The InputFrame ring the App attaches to its SimDriver: one per process,
// never destroyed, registered as a dump stream (kind 3).
[[nodiscard]] InputFrameRing& DumpInputRing();

// The session header (M12): one per process, never destroyed, registered as
// a dump stream (kind 6, crash::StreamType::kSessionHeader) with the
// others. The App begins it at boot step 10 (the module, the scene and its
// hash, the seed, the tick rate, the build, the first tick and its place in
// DumpInputRing) and publishes the last tick's hash after each frame's
// ticks, so tools/se_replay --from-dump can replay the session from a dump.
[[nodiscard]] SessionHeaderStream& DumpSessionHeader();

// The editor's Play-start bytes as a dump stream while Play runs
// (docs/milestone-specs.md M12 and "Integration (M10a + M10b + M12 + M14)";
// M10 keeps the bytes, the integration wires this hook): Attach when Play
// starts copies the bytes behind a kind-7 header (record size 1, capacity
// and head the byte count, name "play-start") and registers them as
// crash::StreamType::kPlayStartBytes; Detach when Play stops (and the
// destructor) unregisters the stream and frees the copy. Attaching again
// replaces the bytes. Main only. Without crash capture (stub builds,
// --no-crash-capture) the stream is still registered and unregistered, so
// the editor needs no switches; only dumps are missing.
class PlayStartStream {
 public:
  PlayStartStream() noexcept;
  PlayStartStream(PlayStartStream&& other) noexcept;
  PlayStartStream& operator=(PlayStartStream&& other) noexcept;
  PlayStartStream(const PlayStartStream&) = delete;
  PlayStartStream& operator=(const PlayStartStream&) = delete;
  ~PlayStartStream();

  // kInvalidArgument for no bytes, or while another PlayStartStream holds
  // the stream type.
  [[nodiscard]] std::expected<void, Error> Attach(
      std::span<const std::byte> bytes);
  // Idempotent.
  void Detach() noexcept;
  [[nodiscard]] bool Attached() const noexcept { return block_ != nullptr; }
  // The registered region (header and bytes); empty while detached.
  [[nodiscard]] std::span<const std::byte> StreamBytes() const noexcept;

 private:
  std::unique_ptr<std::byte[]> block_;
  std::size_t size_ = 0;
};

// Boot step 2: the log worker (role kLogWorker), whose sinks are the
// rotating file |log_path| (empty: none), stderr at config.log_level and
// the panel ring. It starts before any other engine thread. When the file
// cannot be opened the worker runs without it and logs why.
class Logging {
 public:
  Logging() noexcept;
  Logging(Logging&& other) noexcept;
  Logging& operator=(Logging&& other) noexcept;
  Logging(const Logging&) = delete;
  Logging& operator=(const Logging&) = delete;
  ~Logging();

  // Shutdown step 16: SDL's log output to stderr, then drain, flush and
  // stop the worker (spdlog::shutdown). SE_LOG_* writes synchronously from
  // then on. Idempotent.
  void Stop() noexcept;
  [[nodiscard]] bool Running() const noexcept;
  // M10a's log panel: the panel ring's lines after |serial| (see
  // LogWorker::TryCopyPanelLinesSince), and the worker's counts.
  bool TryCopyPanelLinesSince(std::uint64_t& serial,
                              std::vector<std::string>& lines) const;
  [[nodiscard]] LogWorkerStats WorkerStats() const noexcept;
  // The log file, or empty when the worker writes none.
  [[nodiscard]] const std::string& FilePath() const noexcept {
    return file_path_;
  }

 private:
  friend std::expected<Logging, Error> StartLogging(const AppConfig& config,
                                                    std::string_view log_path);
  std::unique_ptr<LogWorker> worker_;
  std::string file_path_;
};
[[nodiscard]] std::expected<Logging, Error> StartLogging(
    const AppConfig& config, std::string_view log_path);

// What the watchdog reads (atomics only; docs/design/mt-design.md "Data
// Flow": iterate counter, Suspended, Modal and heartbeat flags).
using WatchdogInputs = crash::WatchdogInputs;

// Boot step 8: the machine's CPU topology and the pool sizes it gives.
struct CpuInfo {
  int logical_cores = 0;
  int physical_cores = 0;
  // ForkPoolSize: the most fork-join workers M14 may start (the pool runs
  // with none until then).
  int fork_pool_size = 0;
  // PerformanceOnlyCount: workers for barrier-heavy fork sites.
  int performance_only = 0;
  bool homogeneous = true;
};
[[nodiscard]] std::expected<CpuInfo, Error> ReadCpuTopology();

// The vblank sampler at boot step 9: on Windows, for a window, following
// |monitor| (the window's; ADR-056).
struct VblankStart {
  bool enabled = false;
  MonitorHandle monitor = nullptr;
};

// Boot step 9: the helper threads. M4: the watchdog (role kWatchdog, 1 s
// period on the unbiased clock, the 30 s boot budget from
// |boot_start_ns|, armed after the first iterate; M12: a hang after 5
// periods without progress, one dump per episode; config.watchdog_period_ms
// shortens the period in test builds). M6b: the vblank sampler (role
// kVblankSampler, HighQoS) before it; a sampler that cannot start is logged
// and the pacer runs without it. M9 and M8 add the lane and the input
// sampler here.
class HelperThreads {
 public:
  HelperThreads() noexcept;
  HelperThreads(HelperThreads&& other) noexcept;
  HelperThreads& operator=(HelperThreads&& other) noexcept;
  HelperThreads(const HelperThreads&) = delete;
  HelperThreads& operator=(const HelperThreads&) = delete;
  ~HelperThreads();

  // Shutdown step 17: stops the samplers if step 9 did not, then joins the
  // watchdog, the last engine thread. Idempotent.
  void Stop() noexcept;
  // Shutdown step 9: the vblank sampler. Idempotent. Returns how the
  // sampler ended (kDetached: still in its wait, left to end on its own).
  VblankStop StopSamplers() noexcept;
  // The vblank sampler, or nullptr (none started, or stopped).
  [[nodiscard]] VblankSampler* Vblank() const noexcept { return vblank_.get(); }
  // The first iterate ran: the watchdog leaves its boot budget.
  void ArmWatchdog() noexcept;
  // Shutdown step 3: the quit drain is over; the watchdog's 5 s deadline
  // starts.
  void StartShutdownDeadline() noexcept;
  // One dump written by the watchdog thread (main only); false when it
  // cannot be queued.
  bool RequestDump(std::string_view reason) noexcept;
  // Dumps the watchdog has written, or failed to write.
  [[nodiscard]] std::uint64_t DumpsFinished() const noexcept;
  // The watchdog's counters (M12: hangs, their dumps and ends); zeros
  // without a watchdog. Any thread.
  [[nodiscard]] crash::WatchdogStats WatchdogStats() const noexcept;

 private:
  friend std::expected<HelperThreads, Error> StartHelperThreads(
      const AppConfig& config, const WatchdogInputs& inputs,
      std::uint64_t boot_start_ns, const VblankStart& vblank);
  std::unique_ptr<VblankSampler> vblank_;
  std::unique_ptr<crash::Watchdog> watchdog_;
};
[[nodiscard]] std::expected<HelperThreads, Error> StartHelperThreads(
    const AppConfig& config, const WatchdogInputs& inputs,
    std::uint64_t boot_start_ns, const VblankStart& vblank = {});

// The unbiased clock (se::UnbiasedNowNs) for the watchdog's boot budget.
[[nodiscard]] std::uint64_t BootClockNs() noexcept;

// Engine threads still running: the ThreadRegistry's live slots other than
// main's (the init-failure tests require 0). The vblank sampler counts.
[[nodiscard]] std::uint32_t EngineThreadsRunning() noexcept;

// SDL_AppQuit after a failed init: only logging shuts down. The App's
// teardown has already stopped the log worker; this flushes stderr.
void ShutdownLoggingOnly() noexcept;

}  // namespace se::integration

#endif  // SE_RUNTIME_INTEGRATION_H_
