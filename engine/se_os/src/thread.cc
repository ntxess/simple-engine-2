// Copyright 2026 The simple-engine-2 Authors.
//
// se-fault-boundary: std::jthread reports a thread the OS cannot create
// with std::system_error; ThreadStarter::Start turns it into an Error
// (ADR-030). Nothing else here catches.

#include "se_os/thread.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <format>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/profile.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace se {
namespace {

// Each kind of soft failure is logged once per process.
enum class SoftFailure : std::uint8_t {
  kOsName,
  kQos,
  kCpuSet,
  kCount,
};

std::atomic<std::uint32_t> g_logged_failures{0};

bool FirstFailure(SoftFailure kind) noexcept {
  const std::uint32_t bit = 1U << static_cast<std::uint32_t>(kind);
  return (g_logged_failures.fetch_or(bit, std::memory_order_relaxed) & bit) ==
         0;
}

#if defined(_WIN32)

std::wstring Widen(std::string_view text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(std::max(size, 0)), L'\0');
  if (size > 0) {
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), size);
  }
  return wide;
}

std::string Narrow(const wchar_t* text) {
  if (text == nullptr || *text == L'\0') return {};
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string narrow(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), size, nullptr,
                      nullptr);
  narrow.resize(static_cast<std::size_t>(size - 1));
  return narrow;
}

std::expected<std::string, Error> ThreadDescription(HANDLE thread) {
  PWSTR description = nullptr;
  const HRESULT result = GetThreadDescription(thread, &description);
  if (FAILED(result)) {
    return Fail(ErrorCode::kIo,
                std::format("GetThreadDescription failed (0x{:08x})",
                            static_cast<std::uint32_t>(result)));
  }
  std::string name = Narrow(description);
  LocalFree(description);
  return name;
}

bool SetOsName(std::string_view name) noexcept {
  return SUCCEEDED(
      SetThreadDescription(GetCurrentThread(), Widen(name).c_str()));
}

#else

// Linux keeps 15 characters and a NUL.
constexpr std::size_t kLinuxNameSize = 16;

std::expected<std::string, Error> PthreadName(pthread_t thread) {
  char buffer[kLinuxNameSize] = {};
  if (const int error = pthread_getname_np(thread, buffer, sizeof(buffer));
      error != 0) {
    return Fail(ErrorCode::kIo,
                std::format("pthread_getname_np failed ({})", error));
  }
  return std::string(buffer);
}

bool SetOsName(std::string_view name) noexcept {
  char buffer[kLinuxNameSize] = {};
  const std::size_t size = std::min(name.size(), kLinuxNameSize - 1);
  name.copy(buffer, size);
  return pthread_setname_np(pthread_self(), buffer) == 0;
}

#endif

}  // namespace

std::uint64_t CurrentOsThreadIdFromOs() noexcept {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return static_cast<std::uint64_t>(gettid());
#endif
}

std::expected<std::string, Error> CurrentOsThreadName() {
#if defined(_WIN32)
  return ThreadDescription(GetCurrentThread());
#else
  return PthreadName(pthread_self());
#endif
}

std::expected<std::string, Error> OsThreadName(Thread& thread) {
  if (!thread.Joinable()) {
    return Fail(ErrorCode::kInvalidArgument, "the thread is not running");
  }
#if defined(_WIN32)
  return ThreadDescription(static_cast<HANDLE>(thread.NativeHandle()));
#else
  return PthreadName(thread.NativeHandle());
#endif
}

RoleScope::RoleScope(const ThreadSpec& spec)
    : previous_role_(CurrentRole()),
      slot_(spec.role, spec.name, CurrentOsThreadIdFromOs()) {
  if (auto name = CurrentOsThreadName(); name.has_value()) {
    previous_os_name_ = std::move(*name);
  }
  name_applied_ = SetOsName(spec.name);
  if (!name_applied_ && FirstFailure(SoftFailure::kOsName)) {
    SE_LOG_WARN("could not set the OS name of thread {}", spec.name);
  }
  SetCurrentRole(spec.role);
  ApplyQos(spec.qos);
  ApplyCpuSet(spec.cpus);
  profile::SetThreadName(spec.name);
}

RoleScope::~RoleScope() {
  RestoreCpuSet();
  RestoreQos();
  SetCurrentRole(previous_role_);
  if (name_applied_) static_cast<void>(SetOsName(previous_os_name_));
  // slot_ is released after this body: the breadcrumb returns to the
  // thread's own word with the role restored.
}

#if defined(_WIN32)

void RoleScope::ApplyQos(Qos qos) {
  qos_ = qos;
  if (qos == Qos::kDefault) return;
  THREAD_POWER_THROTTLING_STATE state{};
  state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  state.StateMask =
      qos == Qos::kEco ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
  qos_applied_ = SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling,
                                      &state, sizeof(state)) != FALSE;
  qos_changed_ = qos_applied_;
  if (!qos_applied_ && FirstFailure(SoftFailure::kQos)) {
    SE_LOG_WARN("could not set the QoS of this thread (error {})",
                GetLastError());
  }
}

void RoleScope::RestoreQos() const noexcept {
  if (!qos_changed_) return;
  // No override: the system manages throttling again.
  THREAD_POWER_THROTTLING_STATE state{};
  state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  static_cast<void>(SetThreadInformation(
      GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state)));
}

void RoleScope::ApplyCpuSet(const CpuSetRequest& cpus) {
  if (cpus.Empty()) return;
  const HANDLE thread = GetCurrentThread();
  ULONG required = 0;
  std::vector<ULONG> previous;
  if (GetThreadSelectedCpuSets(thread, nullptr, 0, &required) == FALSE &&
      GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
    previous.resize(required);
    if (GetThreadSelectedCpuSets(thread, previous.data(), required,
                                 &required) == FALSE) {
      previous.clear();
    }
  }
  const std::vector<ULONG> ids(cpus.os_ids.begin(), cpus.os_ids.end());
  cpus_applied_ =
      SetThreadSelectedCpuSets(thread, ids.data(),
                               static_cast<ULONG>(ids.size())) != FALSE;
  cpus_changed_ = cpus_applied_;
  previous_cpus_.assign(previous.begin(), previous.end());
  if (!cpus_applied_ && FirstFailure(SoftFailure::kCpuSet)) {
    SE_LOG_WARN("could not set the CPU set of this thread (error {})",
                GetLastError());
  }
}

void RoleScope::RestoreCpuSet() const noexcept {
  if (!cpus_changed_) return;
  // No allocation here (it runs in the destructor): a longer previous
  // selection than the buffer holds is cleared instead.
  std::array<ULONG, 256> ids{};
  const std::size_t count =
      previous_cpus_.size() <= ids.size() ? previous_cpus_.size() : 0;
  std::copy_n(previous_cpus_.begin(), count, ids.begin());
  static_cast<void>(SetThreadSelectedCpuSets(GetCurrentThread(),
                                             count == 0 ? nullptr : ids.data(),
                                             static_cast<ULONG>(count)));
}

#else

// Linux: the nice value is per thread (setpriority on the thread id).
void RoleScope::ApplyQos(Qos qos) {
  qos_ = qos;
  if (qos != Qos::kEco) return;
  const auto tid = static_cast<id_t>(gettid());
  errno = 0;
  const int nice = getpriority(PRIO_PROCESS, tid);
  qos_applied_ =
      errno == 0 && setpriority(PRIO_PROCESS, tid, std::min(nice + 5, 19)) == 0;
  qos_changed_ = qos_applied_;
  previous_nice_ = nice;
  if (!qos_applied_ && FirstFailure(SoftFailure::kQos)) {
    SE_LOG_WARN("could not lower the priority of this thread (errno {})",
                errno);
  }
}

void RoleScope::RestoreQos() const noexcept {
  if (!qos_changed_) return;
  // Raising the priority again needs CAP_SYS_NICE or RLIMIT_NICE; without
  // them the thread keeps the lower priority until it exits.
  static_cast<void>(
      setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), previous_nice_));
}

void RoleScope::ApplyCpuSet(const CpuSetRequest& cpus) {
  if (cpus.Empty()) return;
  cpu_set_t previous;
  CPU_ZERO(&previous);
  const bool have_previous =
      pthread_getaffinity_np(pthread_self(), sizeof(previous), &previous) == 0;
  cpu_set_t wanted;
  CPU_ZERO(&wanted);
  bool valid = true;
  for (const std::uint32_t cpu : cpus.os_ids) {
    if (cpu >= CPU_SETSIZE) {
      valid = false;
      break;
    }
    CPU_SET(cpu, &wanted);
  }
  cpus_applied_ =
      valid && have_previous &&
      pthread_setaffinity_np(pthread_self(), sizeof(wanted), &wanted) == 0;
  cpus_changed_ = cpus_applied_;
  previous_cpus_.clear();
  if (cpus_applied_) {
    for (std::uint32_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (CPU_ISSET(cpu, &previous)) previous_cpus_.push_back(cpu);
    }
  }
  if (!cpus_applied_ && FirstFailure(SoftFailure::kCpuSet)) {
    SE_LOG_WARN("could not set the CPU affinity of this thread");
  }
}

void RoleScope::RestoreCpuSet() const noexcept {
  if (!cpus_changed_) return;
  cpu_set_t previous;
  CPU_ZERO(&previous);
  for (const std::uint32_t cpu : previous_cpus_) CPU_SET(cpu, &previous);
  static_cast<void>(
      pthread_setaffinity_np(pthread_self(), sizeof(previous), &previous));
}

#endif

std::expected<Thread, Error> ThreadStarter::Start(const ThreadSpec& spec,
                                                  ThreadBody body) {
  if (spec.role == ThreadRole::kMain) {
    return Fail(ErrorCode::kInvalidArgument,
                "the main thread is SDL's; it takes a RoleScope instead");
  }
  // The thread owns copies of the spec's name and CPU set.
  struct OwnedSpec {
    ThreadRole role;
    std::string name;
    Qos qos;
    CpuSetRequest cpus;
  };
  OwnedSpec owned{spec.role, std::string(spec.name), spec.qos, spec.cpus};
  std::string name = owned.name;
  try {
    // An exception that escapes a thread's body is a fatal fault: the entry
    // is noexcept, so std::terminate runs at the throw and the crash
    // handler records it there (the threaded pool and the SDL callbacks
    // have their own boundaries).
    // NOLINTNEXTLINE(bugprone-exception-escape)
    auto entry = [owned = std::move(owned), run = std::move(body)](
                     std::stop_token stop) mutable noexcept {
      const RoleScope scope(ThreadSpec{.role = owned.role,
                                       .name = owned.name,
                                       .qos = owned.qos,
                                       .cpus = owned.cpus});
      run(std::move(stop));
    };
    std::jthread thread(std::move(entry));
    return Thread(std::move(thread));
  } catch (const std::system_error& error) {
    return Fail(ErrorCode::kIo, std::format("could not start thread {}: {}",
                                            name, error.what()));
  }
}

}  // namespace se
