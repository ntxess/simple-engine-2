// Copyright 2026 The simple-engine-2 Authors.
//
// The crashpad backend on Windows (ADR-033). It uses crashpad's client API
// directly (CrashpadClient, CrashpadInfo, CrashReportDatabase), not
// sentry_init: registered streams need
// CrashpadInfo::AddUserDataMinidumpStream, and nothing may be uploaded.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "base/files/file_path.h"
#include "base/strings/utf_string_conversions.h"
#include "client/annotation.h"
#include "client/annotation_list.h"
#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/settings.h"
#include "client/simulate_crash.h"
#include "se_core/error.h"
#include "se_crash/crash.h"
#include "src/crash_internal.h"

namespace se::crash {
namespace {

// Annotation objects live in this module's CrashpadInfo; the handler reads
// them at dump time. Constant-initialised and trivially destructible, so
// they outlive every other static.
constinit crashpad::StringAnnotation<kMaxReasonLength> g_dump_reason(
    "se.dump_reason");
constinit crashpad::StringAnnotation<kMaxReasonLength> g_terminate_reason(
    "se.terminate");

// Set once StartHandler has been called: crashpad's process-wide state
// cannot be initialised twice.
std::atomic<bool> g_launched{false};
// True while a CrashGuard is alive.
std::atomic<bool> g_running{false};

// Serialises DumpWithoutCrash requests (crashpad allows one at a time) and
// keeps their reasons apart.
std::mutex& DumpMutex() {
  static std::mutex* const kMutex = new std::mutex();
  return *kMutex;
}

// Guards CrashpadInfo's stream list, which crashpad does not lock.
std::mutex& StreamMutex() {
  static std::mutex* const kMutex = new std::mutex();
  return *kMutex;
}

std::string_view Truncated(std::string_view text) {
  return text.substr(
      0, text.size() < kMaxReasonLength ? text.size() : kMaxReasonLength);
}

bool IsFile(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace

class CrashGuard::Impl {
 public:
  crashpad::CrashpadClient client;
};

CrashGuard::CrashGuard(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
CrashGuard::CrashGuard(CrashGuard&& other) noexcept = default;
CrashGuard& CrashGuard::operator=(CrashGuard&& other) noexcept {
  if (this != &other) {
    // A guard that is replaced ends crash capture like a destroyed one.
    if (impl_ != nullptr) {
      g_running.store(false, std::memory_order_release);
      internal::RestoreTerminateHandler();
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}

CrashGuard::~CrashGuard() {
  if (impl_ == nullptr) {
    return;
  }
  g_running.store(false, std::memory_order_release);
  internal::RestoreTerminateHandler();
  // Wait for a dump in progress: the client must outlive it.
  const std::scoped_lock lock(DumpMutex());
  impl_.reset();
}

std::expected<CrashGuard, Error> Start(const CrashConfig& config) {
  if (config.database_dir.empty() || config.handler_path.empty()) {
    return Fail(ErrorCode::kInvalidArgument,
                "crash capture needs a database directory and a handler path");
  }
  if (g_launched.load(std::memory_order_acquire)) {
    return Fail(ErrorCode::kInvalidArgument,
                "crash capture was already started in this process");
  }
  const std::wstring handler = base::UTF8ToWide(config.handler_path);
  if (!IsFile(handler)) {
    Error error{ErrorCode::kNotFound,
                std::format("crash handler not found: {}", config.handler_path),
                config.handler_path, 0, 0};
    return std::unexpected(std::move(error));
  }
  const base::FilePath database(base::UTF8ToWide(config.database_dir));
  {
    const std::unique_ptr<crashpad::CrashReportDatabase> db =
        crashpad::CrashReportDatabase::Initialize(database);
    if (db == nullptr || db->GetSettings() == nullptr ||
        !db->GetSettings()->SetUploadsEnabled(false)) {
      Error error{
          ErrorCode::kIo,
          std::format("cannot open the crash database {}", config.database_dir),
          config.database_dir, 0, 0};
      return std::unexpected(std::move(error));
    }
  }
  if (g_launched.exchange(true, std::memory_order_acq_rel)) {
    return Fail(ErrorCode::kInvalidArgument,
                "crash capture was already started in this process");
  }

  // StartHandler installs crashpad's process-wide handlers before it
  // launches the handler; put the previous ones back if the launch fails.
  const LPTOP_LEVEL_EXCEPTION_FILTER previous_filter =
      SetUnhandledExceptionFilter(nullptr);
  SetUnhandledExceptionFilter(previous_filter);
  const auto previous_abort = std::signal(SIGABRT, SIG_DFL);
  std::signal(SIGABRT, previous_abort);

  auto impl = std::make_unique<CrashGuard::Impl>();
  // No URL and no metrics directory: nothing is uploaded or recorded
  // besides the dumps. Synchronous, so failure is known here.
  const bool started = impl->client.StartHandler(
      base::FilePath(handler), database, base::FilePath(), /*url=*/"",
      /*http_proxy=*/"", config.annotations, /*arguments=*/{},
      /*restartable=*/false, /*asynchronous_start=*/false);
  if (!started) {
    impl.reset();  // removes crashpad's vectored handler
    SetUnhandledExceptionFilter(previous_filter);
    std::signal(SIGABRT, previous_abort);
    Error error{
        ErrorCode::kIo,
        std::format("crash handler did not start: {}", config.handler_path),
        config.handler_path, 0, 0};
    return std::unexpected(std::move(error));
  }

  // Add the annotations to the list now, so that setting them later (on a
  // terminating thread) neither allocates nor registers anything.
  crashpad::AnnotationList::Register();
  g_dump_reason.Set(std::string_view());
  g_terminate_reason.Set(std::string_view());
  internal::InstallTerminateHandler();
  g_running.store(true, std::memory_order_release);
  return CrashGuard(std::move(impl));
}

bool Running() noexcept { return g_running.load(std::memory_order_acquire); }

std::expected<void, Error> RegisterStream(StreamType type, const void* data,
                                          std::size_t size) {
  const std::scoped_lock lock(StreamMutex());
  if (auto reserved = internal::ReserveStream(type, data, size); !reserved) {
    return reserved;
  }
  // The entry is linked in with its fields set, so the handler reading the
  // list at dump time sees either the old list or the new one.
  if (crashpad::CrashpadInfo::GetCrashpadInfo()->AddUserDataMinidumpStream(
          static_cast<std::uint32_t>(type), data, size) == nullptr) {
    internal::ReleaseStream(type);
    return Fail(ErrorCode::kIo, "crashpad did not register the dump stream");
  }
  return {};
}

std::expected<void, Error> DumpWithoutCrash(std::string_view reason) {
  if (!Running()) {
    return Fail(ErrorCode::kUnsupported, "crash capture is not running");
  }
  const std::scoped_lock lock(DumpMutex());
  if (!Running()) {
    return Fail(ErrorCode::kUnsupported, "crash capture is not running");
  }
  g_dump_reason.Set(Truncated(reason));
  // Captures this thread's context and waits until the handler has written
  // the dump (crashpad's dump_completed event).
  CRASHPAD_SIMULATE_CRASH();
  g_dump_reason.Clear();
  return {};
}

namespace internal {

void RecordTerminateReason(std::string_view reason) {
  g_terminate_reason.Set(Truncated(reason));
}

}  // namespace internal
}  // namespace se::crash
