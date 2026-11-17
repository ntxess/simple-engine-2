// Copyright 2026 The simple-engine-2 Authors.

#include "src/hang_test.h"

#include <SDL3/SDL_timer.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "se_core/log.h"
#include "se_core/schedule.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"
#include "se_os/unbiased_clock.h"
#include "se_platform/driver_wait.h"
#include "se_runtime/app_config.h"
#include "se_sim/game_module.h"

namespace se {
namespace {

constexpr std::uint64_t kNsPerMs = 1'000'000;

struct HangTestSystem {
  static constexpr std::string_view kName = "HangTest";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner);
};

// The wrapped module and the hook; one App at a time.
struct HangTestState {
  const GameModule* base = nullptr;
  GameModule wrapped{};
  HangTestHook hook{};
  SystemId system = 0;
  std::atomic<bool> fired{false};
};

HangTestState& State() {
  static auto* const kState = new HangTestState();
  return *kState;
}

void RegisterSystems(Schedule& schedule) {
  HangTestState& state = State();
  if (state.base->register_systems != nullptr) {
    state.base->register_systems(schedule);
  }
  state.system = schedule.Add(Phase::kCleanup, HangTestSystem{});
}

void HangTestSystem::Run(World& /*world*/, const TickContext& context) {
  HangTestState& state = State();
  const HangTestHook& hook = state.hook;
  if (static_cast<std::int64_t>(context.input.tick) != hook.after_ticks ||
      state.fired.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  SE_LOG_WARN(
      "hang test: main loops {} in system '{}' (id {}, phase Cleanup) at "
      "tick {}{}",
      hook.for_ms > 0 ? std::format("for {} ms", hook.for_ms)
                      : std::string("forever"),
      HangTestSystem::kName, state.system, context.input.tick,
      hook.in_driver ? ", inside a driver wait" : "");
  std::optional<DriverWaitScope> in_driver;
  if (hook.in_driver) in_driver.emplace();
  const std::uint64_t start = UnbiasedNowNs();
  for (;;) {
    if (hook.for_ms > 0 &&
        UnbiasedNowNs() - start >= std::uint64_t{hook.for_ms} * kNsPerMs) {
      break;
    }
    SDL_DelayNS(kNsPerMs);
  }
  in_driver.reset();
  SE_LOG_WARN("hang test: main leaves the loop after {} ms",
              (UnbiasedNowNs() - start) / kNsPerMs);
}

}  // namespace

const GameModule* HangTestModule(const GameModule* module,
                                 const HangTestHook& hook) {
  if (!kRuntimeTestHooks || hook.after_ticks <= 0) return module;
  HangTestState& state = State();
  state.base = module;
  state.hook = hook;
  state.fired.store(false, std::memory_order_relaxed);
  state.wrapped = *module;
  state.wrapped.register_systems = &RegisterSystems;
  return &state.wrapped;
}

}  // namespace se
