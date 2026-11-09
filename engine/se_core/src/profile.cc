// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/profile.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string_view>

#if SE_TRACY
// Tracy's client headers only: tracy/Tracy.hpp defines macros (FrameMark,
// FrameMarkNamed) that clash with the functions below.
#include <client/TracyProfiler.hpp>
#include <client/TracyScoped.hpp>
#include <common/TracySystem.hpp>
#endif

namespace se::profile {
namespace {

std::atomic<bool> g_started{false};
std::atomic<bool> g_ever_started{false};

// The calling thread's name for Tracy, applied by its first profiled event
// after the start.
constexpr std::size_t kThreadNameSize = 32;
thread_local char t_thread_name[kThreadNameSize] = {};
thread_local bool t_name_pending = false;

#if SE_TRACY

static_assert(sizeof(SourceLocation) == sizeof(tracy::SourceLocationData));
static_assert(offsetof(SourceLocation, name) ==
              offsetof(tracy::SourceLocationData, name));
static_assert(offsetof(SourceLocation, function) ==
              offsetof(tracy::SourceLocationData, function));
static_assert(offsetof(SourceLocation, file) ==
              offsetof(tracy::SourceLocationData, file));
static_assert(offsetof(SourceLocation, line) ==
              offsetof(tracy::SourceLocationData, line));
static_assert(offsetof(SourceLocation, color) ==
              offsetof(tracy::SourceLocationData, color));
static_assert(sizeof(tracy::ScopedZone) <= Zone::kStorageSize);
static_assert(alignof(tracy::ScopedZone) <= 8);

void ApplyPendingName() noexcept {
  if (t_name_pending) {
    t_name_pending = false;
    tracy::SetThreadName(t_thread_name);
  }
}

#endif

}  // namespace

bool Started() noexcept { return g_started.load(std::memory_order_acquire); }

bool EverStarted() noexcept {
  return g_ever_started.load(std::memory_order_acquire);
}

bool Start() noexcept {
#if SE_TRACY
  bool expected = false;
  if (!g_ever_started.compare_exchange_strong(expected, true)) return false;
  tracy::StartupProfiler();
  g_started.store(true, std::memory_order_release);
  ApplyPendingName();
  return true;
#else
  return false;
#endif
}

void Stop() noexcept {
#if SE_TRACY
  if (g_started.exchange(false, std::memory_order_acq_rel)) {
    tracy::ShutdownProfiler();
  }
#endif
}

Zone::Zone(const SourceLocation* location) noexcept {
#if SE_TRACY
  if (!Started()) return;
  ApplyPendingName();
  new (storage_) tracy::ScopedZone(
      reinterpret_cast<const tracy::SourceLocationData*>(location));
  active_ = true;
#else
  static_cast<void>(location);
#endif
}

Zone::Zone(std::string_view name, const char* file, std::uint32_t line,
           const char* function) noexcept {
#if SE_TRACY
  if (!Started()) return;
  ApplyPendingName();
  new (storage_)
      tracy::ScopedZone(line, file, std::strlen(file), function,
                        std::strlen(function), name.data(), name.size(), 0);
  active_ = true;
#else
  static_cast<void>(name);
  static_cast<void>(file);
  static_cast<void>(line);
  static_cast<void>(function);
#endif
}

Zone::~Zone() {
#if SE_TRACY
  if (active_) {
    std::launder(reinterpret_cast<tracy::ScopedZone*>(storage_))->~ScopedZone();
  }
#endif
}

void FrameMark() noexcept {
#if SE_TRACY
  if (!Started()) return;
  ApplyPendingName();
  tracy::Profiler::SendFrameMark(nullptr);
#endif
}

void FrameMarkNamed(const char* name) noexcept {
#if SE_TRACY
  if (!Started()) return;
  ApplyPendingName();
  tracy::Profiler::SendFrameMark(name);
#else
  static_cast<void>(name);
#endif
}

void Plot(const char* name, double value) noexcept {
#if SE_TRACY
  if (!Started()) return;
  ApplyPendingName();
  tracy::Profiler::PlotData(name, value);
#else
  static_cast<void>(name);
  static_cast<void>(value);
#endif
}

void Plot(const char* name, std::int64_t value) noexcept {
#if SE_TRACY
  if (!Started()) return;
  ApplyPendingName();
  tracy::Profiler::PlotData(name, value);
#else
  static_cast<void>(name);
  static_cast<void>(value);
#endif
}

void Message(std::string_view text) noexcept {
#if SE_TRACY
  if (!Started()) return;
  ApplyPendingName();
  tracy::Profiler::LogString(tracy::MessageSourceType::User,
                             tracy::MessageSeverity::Info, 0, 0, text.size(),
                             text.data());
#else
  static_cast<void>(text);
#endif
}

void SetThreadName(std::string_view name) noexcept {
  const std::size_t size = std::min(name.size(), kThreadNameSize - 1);
  name.copy(t_thread_name, size);
  t_thread_name[size] = '\0';
  t_name_pending = true;
#if SE_TRACY
  if (Started()) ApplyPendingName();
#endif
}

}  // namespace se::profile

#if SE_TRACY && defined(_WIN32)
// TRACY_DBGHELP_LOCK=SeDbgHelp: Tracy serialises its own DbgHelp calls
// (symbol resolution) through these. Engine code makes no DbgHelp calls in
// process (crash dumps are written out of process).
namespace {
std::mutex& DbgHelpMutex() {
  static std::mutex* const kMutex = new std::mutex();
  return *kMutex;
}
}  // namespace

extern "C" {
void SeDbgHelpInit() {}
void SeDbgHelpLock() { DbgHelpMutex().lock(); }
void SeDbgHelpUnlock() { DbgHelpMutex().unlock(); }
}
#endif
