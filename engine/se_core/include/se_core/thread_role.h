// Copyright 2026 The simple-engine-2 Authors.
//
// Thread roles and the two capabilities of ADR-007.
//
//   g_main       the SDL main thread: window, GPU, ImGui. Acquired once by
//                RegisterMainThread() at startup.
//   g_sim_owner  ownership of the running Worlds. The main thread holds it in
//                inline mode (InlineOwnerScope); the M17 sim thread holds it
//                under the threaded driver.
//
// World structural calls require g_sim_owner, so the analysis (clangcl-dev,
// -Werror=thread-safety) rejects them from code that does not hold it, for
// example the body of a fork chunk. The run-time checks compare thread ids
// instead (World::owner()), because se_core may not call SDL (ADR-009).

#ifndef SE_CORE_THREAD_ROLE_H_
#define SE_CORE_THREAD_ROLE_H_

#include <cstdint>
#include <string_view>

#include "se_core/assert.h"
#include "se_core/thread_safety.h"

namespace se {

struct SE_CAPABILITY("role") MainThreadRole {};
struct SE_CAPABILITY("role") WorldOwnerRole {};

// Main-thread-only work (window, GPU, ImGui).
extern MainThreadRole g_main;
// Ownership of the running Worlds.
extern WorldOwnerRole g_sim_owner;

enum class ThreadRole : std::uint8_t {
  kLibrary,  // a thread the engine did not create
  kMain,
  kSim,
  kForkWorker,
  kBackground,
  kLogWorker,
  kWatchdog,
  kInputSampler,
  kVblankSampler,
};

// Records the calling thread as the main thread and sets its role. Called
// once at startup (SDL_AppInit; tools and tests call it from main).
void RegisterMainThread() noexcept SE_ACQUIRE(g_main);

// Ends the main-thread registration at shutdown.
void UnregisterMainThread() noexcept SE_RELEASE(g_main);

// True on the thread that called RegisterMainThread().
[[nodiscard]] bool IsMainThread() noexcept;

// The calling thread's role; kLibrary unless set.
[[nodiscard]] ThreadRole CurrentRole() noexcept;

// Sets the calling thread's role (thread entry points; M4 adds RoleScope).
void SetCurrentRole(ThreadRole role) noexcept;

// A short lower-case name for logs and breadcrumbs ("main", "sim", ...).
[[nodiscard]] std::string_view RoleName(ThreadRole role) noexcept;

// Checks at run time that this is the main thread; afterwards the analysis
// treats g_main as held.
void AssertMainThread() noexcept SE_ASSERT_CAPABILITY(g_main);

// Wakes the main thread's event wait. Declared here and defined by
// se_platform (an SDL user event); test binaries link a null definition.
void WakeMain() noexcept;

// Proof, for the analysis, that the caller holds g_main: only
// OnMainThread() makes one. InlineOwnerScope takes it instead of requiring
// g_main itself, because clang reads a requirement on a scoped capability's
// constructor as adopting that capability: the scope would then release
// g_main when it ends, or the analysis would reject the paths that join
// after it.
class MainThreadProof {
 private:
  MainThreadProof() = default;
  friend MainThreadProof OnMainThread() noexcept;
};

// Asserts the main thread and returns the proof of it.
[[nodiscard]] MainThreadProof OnMainThread() noexcept SE_REQUIRES(g_main);

// Holds g_sim_owner while the main thread steps Worlds in inline mode:
//   const InlineOwnerScope owner(OnMainThread());
// so the analysis requires g_main where the scope is made, and the
// constructor asserts the main thread at run time. The M17 sim thread takes
// g_sim_owner through its own scope. Besides that check the cost is a
// thread-local counter: the Worlds check their owner thread themselves.
class SE_SCOPED_CAPABILITY InlineOwnerScope {
 public:
  explicit InlineOwnerScope(MainThreadProof on_main) noexcept
      SE_ACQUIRE(g_sim_owner);
  ~InlineOwnerScope() SE_RELEASE(g_sim_owner);

  // True while the calling thread is inside an InlineOwnerScope.
  [[nodiscard]] static bool HeldOnThisThread() noexcept;

  InlineOwnerScope(const InlineOwnerScope&) = delete;
  InlineOwnerScope& operator=(const InlineOwnerScope&) = delete;
};

}  // namespace se

// Asserts the calling thread is the registered main thread.
#define SE_ASSERT_MAIN_THREAD() \
  SE_ASSERT_MSG(::se::IsMainThread(), "not on the main thread")

// Asserts the calling thread is not running a fork chunk. Needs
// se_core/jobs.h, which declares ::se::jobs::t_in_fork.
#define SE_ASSERT_NOT_IN_FORK() \
  SE_ASSERT_MSG(!::se::jobs::t_in_fork, "not allowed inside a fork chunk")

#endif  // SE_CORE_THREAD_ROLE_H_
