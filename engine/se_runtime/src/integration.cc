// Copyright 2026 The simple-engine-2 Authors.
//
// The M4 parts in the runtime (integration.h): the main thread's RoleScope,
// crash capture and its dump streams, the log worker, the CPU topology and
// the watchdog.

#include "se_runtime/integration.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/assert.h"
#include "se_core/dump_stream.h"
#include "se_core/error.h"
#include "se_core/input_frame_ring.h"
#include "se_core/log.h"
#include "se_core/session_header.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_crash/crash.h"
#include "se_crash/watchdog.h"
#include "se_log/log_worker.h"
#include "se_os/cpu_topology.h"
#include "se_os/process.h"
#include "se_os/thread.h"
#include "se_os/unbiased_clock.h"
#include "se_platform/sdl_context.h"
#include "se_platform/vblank_sampler.h"
#include "se_runtime/app_config.h"

namespace se::integration {
namespace {

static_assert(kLibraryLogRingIndex == crash::kLibraryLogRingSlot,
              "the library ring's stream type is LogRingStream(64)");
static_assert(kThreadSlotCount <= crash::kMaxLogRingSlots);

// The main thread's RoleScope between EnterMainThread and LeaveMainThread.
RoleScope* g_main_scope = nullptr;
// Dump streams RegisterStream refused (reported in the crash capture's
// status), and fault dumps that could not be written (a fault path cannot
// log; a debugger or a later dump shows the count).
std::atomic<std::uint32_t> g_streams_refused{0};
std::atomic<std::uint32_t> g_fault_dumps_failed{0};

// The process's crash capture: started at most once, never stopped
// (crashpad's exception filter stays until the process exits, and a
// second handler cannot be launched).
struct ProcessCrashCapture {
  bool streams_registered = false;
  bool attempted = false;
  bool active = false;
  std::string status;
  std::unique_ptr<crash::CrashGuard> guard;
};

ProcessCrashCapture& Capture() {
  static auto* const kCapture = new ProcessCrashCapture();
  return *kCapture;
}

// The ThreadRegistry's regions become dump streams as they appear: the
// breadcrumb table, each slot's log ring when the slot is first used, and
// the library ring. Runs under the registry mutex, so it neither logs nor
// starts threads; a refused registration (a duplicate, in tests that boot
// twice) keeps the first.
void RegisterRegistryStream(void* /*context*/, DumpStreamKind kind,
                            std::uint32_t index,
                            std::span<const std::byte> region) {
  const crash::StreamType type = kind == DumpStreamKind::kBreadcrumbs
                                     ? crash::StreamType::kBreadcrumbs
                                     : crash::LogRingStream(index);
  if (!crash::RegisterStream(type, region.data(), region.size())) {
    g_streams_refused.fetch_add(1, std::memory_order_relaxed);
  }
}

void RegisterDumpStreams() {
  ThreadRegistry::Instance().SetStreamHook(&RegisterRegistryStream, nullptr);
  const std::span<const std::byte> ring = DumpInputRing().StreamBytes();
  if (!crash::RegisterStream(crash::StreamType::kInputFrameRing, ring.data(),
                             ring.size())) {
    g_streams_refused.fetch_add(1, std::memory_order_relaxed);
  }
  const std::span<const std::byte> header = DumpSessionHeader().StreamBytes();
  if (!crash::RegisterStream(crash::StreamType::kSessionHeader, header.data(),
                             header.size())) {
    g_streams_refused.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace

// The role is a static token with nothing to lock (thread_role.cc), so the
// analysis is off for the two functions that take and drop it.
void EnterMainThread() noexcept SE_NO_THREAD_SAFETY_ANALYSIS {
  RegisterMainThread();
  if (g_main_scope == nullptr) {
    // Without memory for it, main keeps the role RegisterMainThread set.
    g_main_scope = new (std::nothrow) RoleScope(
        {.role = ThreadRole::kMain, .name = "main", .qos = Qos::kHigh});
  }
}

void LeaveMainThread() noexcept SE_NO_THREAD_SAFETY_ANALYSIS {
  delete g_main_scope;
  g_main_scope = nullptr;
  UnregisterMainThread();
}

std::expected<CrashCapture, Error> StartCrashCapture(
    std::string_view database_dir, std::string_view app_name,
    bool start_handler) {
  ProcessCrashCapture& process = Capture();
  if (!process.streams_registered) {
    process.streams_registered = true;
    RegisterDumpStreams();
  }
  if (!process.attempted && start_handler) {
    process.attempted = true;
    crash::CrashConfig config;
    config.database_dir = std::string(database_dir);
    if (auto dir = ExecutableDir(); dir) {
      config.handler_path = crash::HandlerPathIn(*dir);
    }
    config.annotations = {{"se.app", std::string(app_name)},
                          {"se.test_hooks", kRuntimeTestHooks ? "1" : "0"}};
    auto guard = crash::Start(config);
    if (guard) {
      process.active = true;
      process.guard = std::make_unique<crash::CrashGuard>(std::move(*guard));
      process.status =
          std::format("crash capture on (database {})", config.database_dir);
    } else {
      process.status =
          std::format("crash capture off: {}", guard.error().message);
    }
  }
  CrashCapture capture;
  capture.active_ = process.active && crash::Running();
  capture.status_ = process.attempted || start_handler
                        ? process.status
                        : std::string("crash capture off (--no-crash-capture)");
  if (const std::uint32_t refused =
          g_streams_refused.load(std::memory_order_relaxed);
      refused > 0) {
    capture.status_ += std::format(", {} dump streams refused", refused);
  }
  return capture;
}

void RequestFaultDump(std::string_view reason) noexcept {
  if (!crash::Running()) return;
  if (!crash::DumpWithoutCrash(reason)) {
    g_fault_dumps_failed.fetch_add(1, std::memory_order_relaxed);
  }
}

InputFrameRing& DumpInputRing() {
  static auto* const kRing = new InputFrameRing();
  return *kRing;
}

SessionHeaderStream& DumpSessionHeader() {
  static auto* const kHeader = new SessionHeaderStream();
  return *kHeader;
}

PlayStartStream::PlayStartStream() noexcept = default;

PlayStartStream::PlayStartStream(PlayStartStream&& other) noexcept
    : block_(std::move(other.block_)), size_(std::exchange(other.size_, 0)) {}

PlayStartStream& PlayStartStream::operator=(PlayStartStream&& other) noexcept {
  if (this != &other) {
    Detach();
    block_ = std::move(other.block_);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

PlayStartStream::~PlayStartStream() { Detach(); }

std::expected<void, Error> PlayStartStream::Attach(
    std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return Fail(ErrorCode::kInvalidArgument, "no Play-start bytes to attach");
  }
  if (bytes.size() > std::size_t{UINT32_MAX}) {
    return Fail(ErrorCode::kInvalidArgument,
                "the Play-start bytes are too large for a dump stream");
  }
  Detach();
  const std::size_t size = sizeof(DumpStreamHeader) + bytes.size();
  auto block = std::make_unique<std::byte[]>(size);
  DumpStreamHeader header;
  InitDumpStreamHeader(header, DumpStreamKind::kBytes, 1,
                       static_cast<std::uint32_t>(bytes.size()), "play-start");
  header.head = bytes.size();
  header.os_thread_id = CurrentOsThreadId();
  header.role = static_cast<std::uint8_t>(CurrentRole());
  std::memcpy(block.get(), &header, sizeof(header));
  std::memcpy(block.get() + sizeof(header), bytes.data(), bytes.size());
  if (auto registered = crash::RegisterStream(
          crash::StreamType::kPlayStartBytes, block.get(), size);
      !registered) {
    return registered;
  }
  block_ = std::move(block);
  size_ = size;
  return {};
}

void PlayStartStream::Detach() noexcept {
  if (block_ == nullptr) return;
  // The stream is withdrawn before its memory goes (crash.h). It is
  // registered while attached, so this cannot fail.
  [[maybe_unused]] const bool withdrawn =
      crash::UnregisterStream(crash::StreamType::kPlayStartBytes).has_value();
  SE_ASSERT_MSG(withdrawn, "the Play-start stream was not registered");
  block_.reset();
  size_ = 0;
}

std::span<const std::byte> PlayStartStream::StreamBytes() const noexcept {
  if (block_ == nullptr) return {};
  return {block_.get(), size_};
}

std::expected<Logging, Error> StartLogging(const AppConfig& config,
                                           std::string_view log_path) {
  SetLogLevel(config.log_level);
  LogConfig log_config;
  log_config.file_path = std::string(log_path);
  log_config.stderr_level = config.log_level;
  auto worker = LogWorker::Start(log_config);
  std::string file_failure;
  if (!worker && !log_config.file_path.empty()) {
    // The game runs on without its log file; the reason is logged below.
    file_failure = worker.error().message;
    log_config.file_path.clear();
    worker = LogWorker::Start(log_config);
  }
  if (!worker) return std::unexpected(std::move(worker.error()));
  Logging logging;
  logging.worker_ = std::make_unique<LogWorker>(std::move(*worker));
  logging.file_path_ = log_config.file_path;
  if (!file_failure.empty()) {
    SE_LOG_WARN("logging without a file: {}", file_failure);
  }
  return logging;
}

Logging::Logging() noexcept = default;
Logging::Logging(Logging&& other) noexcept = default;

Logging& Logging::operator=(Logging&& other) noexcept {
  if (this != &other) {
    Stop();
    worker_ = std::move(other.worker_);
    file_path_ = std::move(other.file_path_);
  }
  return *this;
}

Logging::~Logging() { Stop(); }

void Logging::Stop() noexcept {
  if (!worker_) return;
  // SDL may log from any thread until it is shut down; from here on its
  // messages go to stderr directly.
  RouteSdlLogToDefault();
  worker_->Stop();
  worker_.reset();
  std::fflush(stderr);
}

bool Logging::Running() const noexcept {
  return worker_ != nullptr && worker_->Running();
}

std::expected<CpuInfo, Error> ReadCpuTopology() {
  auto topology = se::ReadCpuTopology();
  if (!topology) return std::unexpected(std::move(topology.error()));
  CpuInfo info;
  info.logical_cores = static_cast<int>(topology->logical_count);
  info.physical_cores = static_cast<int>(topology->PhysicalCores());
  info.fork_pool_size = ForkPoolSize(*topology);
  info.performance_only = PerformanceOnlyCount(*topology);
  info.homogeneous = topology->Homogeneous();
  return info;
}

std::expected<HelperThreads, Error> StartHelperThreads(
    const AppConfig& config, const WatchdogInputs& inputs,
    std::uint64_t boot_start_ns, const VblankStart& vblank) {
  HelperThreads threads;
  // The vblank sampler before the watchdog (docs/design/mt-design.md
  // "Booting -> Running"). Not a boot failure when it cannot run: the
  // pacer runs Standard without it.
  if (vblank.enabled && VblankSampler::Supported()) {
    VblankSamplerConfig sampler_config;
    sampler_config.monitor = vblank.monitor;
    auto sampler = VblankSampler::Start(sampler_config);
    if (sampler) {
      threads.vblank_ = std::move(*sampler);
    } else {
      SE_LOG_WARN("no vblank sampler: {}", sampler.error().message);
    }
  }
  crash::WatchdogConfig watchdog_config;
  watchdog_config.inputs = inputs;
  watchdog_config.boot_start_ns = boot_start_ns;
  // A shorter hang period (test builds: the headless hang tests).
  if (kRuntimeTestHooks && config.watchdog_period_ms > 0) {
    watchdog_config.period_ms = config.watchdog_period_ms;
  }
  auto watchdog = crash::Watchdog::Start(watchdog_config);
  if (!watchdog) return std::unexpected(std::move(watchdog.error()));
  threads.watchdog_ = std::make_unique<crash::Watchdog>(std::move(*watchdog));
  return threads;
}

HelperThreads::HelperThreads() noexcept = default;
HelperThreads::HelperThreads(HelperThreads&& other) noexcept = default;

HelperThreads& HelperThreads::operator=(HelperThreads&& other) noexcept {
  if (this != &other) {
    Stop();
    vblank_ = std::move(other.vblank_);
    watchdog_ = std::move(other.watchdog_);
  }
  return *this;
}

HelperThreads::~HelperThreads() { Stop(); }

VblankStop HelperThreads::StopSamplers() noexcept {
  if (!vblank_) return VblankStop::kNotRunning;
  const VblankStop stopped = vblank_->Stop();
  vblank_.reset();
  return stopped;
}

void HelperThreads::Stop() noexcept {
  StopSamplers();
  if (!watchdog_) return;
  watchdog_->Stop();
  watchdog_.reset();
}

void HelperThreads::ArmWatchdog() noexcept {
  if (watchdog_) watchdog_->Arm();
}

void HelperThreads::StartShutdownDeadline() noexcept {
  if (watchdog_) watchdog_->StartShutdownDeadline();
}

bool HelperThreads::RequestDump(std::string_view reason) noexcept {
  return watchdog_ != nullptr && watchdog_->RequestDump(reason);
}

std::uint64_t HelperThreads::DumpsFinished() const noexcept {
  if (!watchdog_) return 0;
  const crash::WatchdogStats stats = watchdog_->Stats();
  return stats.dumps_written + stats.dumps_failed;
}

crash::WatchdogStats HelperThreads::WatchdogStats() const noexcept {
  return watchdog_ ? watchdog_->Stats() : crash::WatchdogStats{};
}

std::uint64_t BootClockNs() noexcept { return UnbiasedNowNs(); }

std::uint32_t EngineThreadsRunning() noexcept {
  const ThreadRegistry& registry = ThreadRegistry::Instance();
  std::uint32_t running = 0;
  for (std::uint32_t slot = 0; slot < kThreadSlotCount; ++slot) {
    if (slot == kMainThreadSlot) continue;
    if (registry.Read(slot).state == ThreadSlotState::kLive) ++running;
  }
  return running;
}

void ShutdownLoggingOnly() noexcept { std::fflush(stderr); }

}  // namespace se::integration
