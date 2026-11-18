// Copyright 2026 The simple-engine-2 Authors.
//
// The phased Schedule (docs/design/costfree-end-state.md "Systems, schedule
// and gameplay rules"). A game module declares it once; the runtime, editor
// Play sessions, se_replay and tests share it.
//
// Systems are stateless structs with a static name and
//   void Run(World& world, const TickContext& context)
//   SE_REQUIRES(g_sim_owner);
// All simulation state lives in the World.
//
// Step(world, input, jobs), one tick:
//   1. opens the World's MutationGate and advances its SimClock;
//   2. runs the phases Input, Intent, Movement, Collision, Effects, GameRules
//      and Cleanup in order, the systems of a phase in registration order;
//      the built-in PrevTransformCopy (a fork site) runs at the head of
//      Movement, the built-in Cleanup (drain destroys, FinalizeSpawns,
//      record the hash) at the end of Cleanup;
//   3. sets the thread's breadcrumb (tick, phase, system) as it goes and
//      times every system into a 1024-entry ring.

#ifndef SE_CORE_SCHEDULE_H_
#define SE_CORE_SCHEDULE_H_

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/sim_clock.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"

namespace se {

enum class Phase : std::uint8_t {
  kInput,
  kIntent,
  kMovement,
  kCollision,
  kEffects,
  kGameRules,
  kCleanup,
};
inline constexpr int kPhaseCount = 7;

[[nodiscard]] std::string_view PhaseName(Phase phase) noexcept;

// The built-in PrevTransformCopy's fork site ("se.PrevTransformCopy", grain
// 1024: reads Transform, writes PrevTransform; M14). Disabled unless the
// pool enables it.
[[nodiscard]] const jobs::ForkSiteDecl& PrevTransformCopySite();

// What a system sees besides the World.
struct TickContext {
  const InputFrame& input;
  const SimClock& clock;
  jobs::ForkJoinPool& jobs;
};

template <class S>
concept System = std::is_empty_v<S> && std::is_default_constructible_v<S> &&
                 requires(S system, World& world, const TickContext& context) {
                   { S::kName } -> std::convertible_to<std::string_view>;
                   system.Run(world, context);
                 };

using SystemId = std::uint16_t;
inline constexpr std::size_t kSystemRingSize = 1024;

// Timing of one system: the last kSystemRingSize runs.
struct SystemStats {
  std::string_view name{};
  Phase phase = Phase::kInput;
  bool built_in = false;
  std::array<std::uint32_t, kSystemRingSize> ring_ns{};
  std::uint32_t head = 0;
  std::uint32_t count = 0;
  std::uint64_t runs = 0;
  std::uint32_t last_ns = 0;
  std::uint32_t max_ns = 0;
  // Of the last run, the time inside threaded forks (publish to join, the
  // pool's ThreadedJoinNs): the run's parallel part. 0 while its fork
  // sites run inline (every site ships disabled).
  std::uint32_t last_fork_ns = 0;

  void Record(std::uint64_t ns, std::uint64_t fork_ns = 0) noexcept;
  // Nearest-rank percentile of the ring; 0 when empty.
  [[nodiscard]] std::uint32_t PercentileNs(double p) const;
};

class Schedule {
 public:
  // Ids of the built-in systems in Stats().
  static constexpr SystemId kPrevTransformCopyId = 0;
  static constexpr SystemId kCleanupId = 1;

  Schedule();

  // Registers a system in a phase. Systems hold no state, so the argument
  // only names the type.
  template <System S>
  SystemId Add(Phase phase, S /*system*/ = {}) {
    return AddEntry(phase, S::kName, &RunSystem<S>);
  }

  // No more registrations; Step requires a sealed Schedule.
  void Seal();
  [[nodiscard]] bool Sealed() const noexcept { return sealed_; }

  // Runs one tick of |world|.
  void Step(World& world, const InputFrame& input, jobs::ForkJoinPool& jobs)
      SE_REQUIRES(g_sim_owner);

  // Built-ins first, then the systems in registration order.
  [[nodiscard]] std::span<const SystemStats> Stats() const noexcept {
    return stats_;
  }

 private:
  using RunFn = void (*)(World&, const TickContext&);

  struct Entry {
    Phase phase;
    RunFn run;
    SystemId id;
  };

  template <System S>
  static void RunSystem(World& world, const TickContext& context)
      SE_REQUIRES(g_sim_owner) {
    S system;
    system.Run(world, context);
  }

  SystemId AddEntry(Phase phase, std::string_view name, RunFn run);

  std::vector<Entry> entries_;
  std::vector<SystemStats> stats_;
  bool sealed_ = false;
};

}  // namespace se

#endif  // SE_CORE_SCHEDULE_H_
