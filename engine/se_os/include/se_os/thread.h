// Copyright 2026 The simple-engine-2 Authors.
//
// Engine threads (docs/design/mt-design.md "Threads"). Every engine thread is
// a named role: it is started through StartThread, which runs its body
// inside a RoleScope, and nothing else in engine code creates threads
// (tools/checks/check_banned_api.py rejects raw thread creation outside
// se_os). A thread the engine did not start but that runs engine work (the
// SDL main thread) takes a RoleScope itself.
//
//   auto worker = se::StartThread(
//       {.role = ThreadRole::kLogWorker, .name = "log-worker"},
//       [](std::stop_token stop) { ... });
//
// A RoleScope, for the lifetime of the scope:
//   - names the OS thread (SetThreadDescription; pthread_setname_np, which
//     keeps 15 characters);
//   - sets the thread's role (SetCurrentRole) and so its breadcrumb role;
//   - takes a ThreadRegistry slot, which carries the breadcrumb and the log
//     ring of the thread (slot 0 for the main thread);
//   - applies the QoS: on Windows SetThreadInformation(ThreadPowerThrottling)
//     with kHigh = throttling explicitly off, kEco = on, kDefault = no
//     override; on Linux kEco lowers the thread's priority by raising its
//     nice value by 5, and kHigh and kDefault change nothing;
//   - applies the CPU set (SetThreadSelectedCpuSets; pthread_setaffinity_np)
//     only when one is requested;
//   - names the thread for Tracy through the profile hook.
// Its destructor restores what it changed. Every OS call fails softly: a
// failure is logged once per kind and the scope carries on (QosApplied(),
// CpuSetApplied() and OsNameApplied() report what took effect).

#ifndef SE_OS_THREAD_H_
#define SE_OS_THREAD_H_

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"

namespace se {

enum class Qos : std::uint8_t {
  // No override: the OS decides.
  kDefault,
  // Never throttled (main, fork workers, samplers).
  kHigh,
  // Efficiency (EcoQoS on Windows; a lower priority on Linux).
  kEco,
};

// The logical processors a thread prefers. The ids are the OS's: CPU set
// ids on Windows, CPU numbers on Linux, as CpuTopology reports them. An
// empty request leaves the thread's placement alone.
struct CpuSetRequest {
  std::vector<std::uint32_t> os_ids;

  [[nodiscard]] bool Empty() const noexcept { return os_ids.empty(); }
};

struct ThreadSpec {
  ThreadRole role = ThreadRole::kLibrary;
  // Shown by debuggers, profilers and the logs; at most kThreadNameSize
  // characters are kept (15 for the Linux OS name).
  std::string_view name;
  Qos qos = Qos::kDefault;
  CpuSetRequest cpus;
};

// Gives the calling thread its role for the lifetime of the scope (see the
// file comment). Engine threads get one from StartThread; the main thread
// makes its own after RegisterMainThread().
class RoleScope {
 public:
  explicit RoleScope(const ThreadSpec& spec);
  ~RoleScope();

  RoleScope(const RoleScope&) = delete;
  RoleScope& operator=(const RoleScope&) = delete;

  // The ThreadRegistry slot, or nullopt when the table was full.
  [[nodiscard]] std::optional<std::uint32_t> Slot() const noexcept {
    return slot_.Slot();
  }
  // True when the requested QoS took effect (always for kDefault).
  [[nodiscard]] bool QosApplied() const noexcept { return qos_applied_; }
  // True when the requested CPU set took effect (always for none).
  [[nodiscard]] bool CpuSetApplied() const noexcept { return cpus_applied_; }
  // True when the OS thread name was set.
  [[nodiscard]] bool OsNameApplied() const noexcept { return name_applied_; }

 private:
  // The Apply calls log a failure; the Restore calls run in the destructor
  // and stay silent (a Linux thread may not raise its priority again).
  void ApplyQos(Qos qos);
  void RestoreQos() const noexcept;
  void ApplyCpuSet(const CpuSetRequest& cpus);
  void RestoreCpuSet() const noexcept;

  ThreadRole previous_role_;
  std::string previous_os_name_;
  bool name_applied_ = false;
  ThreadSlotScope slot_;
  Qos qos_ = Qos::kDefault;
  bool qos_applied_ = true;
  bool qos_changed_ = false;
  int previous_nice_ = 0;
  bool cpus_applied_ = true;
  bool cpus_changed_ = false;
  std::vector<std::uint32_t> previous_cpus_;
};

// An engine thread: a std::jthread whose body runs inside a RoleScope. The
// destructor requests a stop and joins.
class Thread {
 public:
  Thread() noexcept = default;
  Thread(Thread&&) noexcept = default;
  Thread& operator=(Thread&&) noexcept = default;
  ~Thread() = default;

  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;

  // Asks the body to return (its stop_token reports it).
  void RequestStop() noexcept { thread_.request_stop(); }
  [[nodiscard]] std::stop_token StopToken() const noexcept {
    return thread_.get_stop_token();
  }
  [[nodiscard]] bool Joinable() const noexcept { return thread_.joinable(); }
  // Waits for the body to return. Does not request a stop.
  void Join() noexcept {
    if (thread_.joinable()) thread_.join();
  }
  [[nodiscard]] std::thread::native_handle_type NativeHandle() noexcept {
    return thread_.native_handle();
  }

 private:
  friend class ThreadStarter;
  explicit Thread(std::jthread thread) noexcept : thread_(std::move(thread)) {}

  std::jthread thread_;
};

using ThreadBody = std::move_only_function<void(std::stop_token)>;

// Starts the non-template part of StartThread (thread.cc, a fault boundary
// for std::jthread's std::system_error).
class ThreadStarter {
 public:
  static std::expected<Thread, Error> Start(const ThreadSpec& spec,
                                            ThreadBody body);
};

// Starts an engine thread running body(std::stop_token) or body() inside a
// RoleScope built from |spec|. The name is copied. Fails when the OS cannot
// create the thread, or for the main role (the main thread is SDL's).
template <class F>
  requires std::is_invocable_v<std::decay_t<F>&, std::stop_token> ||
           std::is_invocable_v<std::decay_t<F>&>
[[nodiscard]] std::expected<Thread, Error> StartThread(const ThreadSpec& spec,
                                                       F&& body) {
  if constexpr (std::is_invocable_v<std::decay_t<F>&, std::stop_token>) {
    return ThreadStarter::Start(spec, ThreadBody(std::forward<F>(body)));
  } else {
    return ThreadStarter::Start(
        spec,
        ThreadBody([run = std::forward<F>(body)](
                       const std::stop_token& /*stop*/) mutable { run(); }));
  }
}

// The calling thread's OS thread id (GetCurrentThreadId; gettid).
[[nodiscard]] std::uint64_t CurrentOsThreadIdFromOs() noexcept;

// The OS name of the calling thread (GetThreadDescription;
// pthread_getname_np), UTF-8.
[[nodiscard]] std::expected<std::string, Error> CurrentOsThreadName();

// The OS name of |thread|.
[[nodiscard]] std::expected<std::string, Error> OsThreadName(Thread& thread);

}  // namespace se

#endif  // SE_OS_THREAD_H_
