// Copyright 2026 The simple-engine-2 Authors.
//
// The helper threads in the App's boot and shutdown order (docs/milestone-
// specs.md "Integration (M8 + M9 + M17a)"; ADR-088), headless (label unit,
// so the WSL sanitizer presets run it): the background lane is a live
// ThreadRegistry slot when the boot ends, the input sampler joins it at the
// first iterate (inside ADR-047's window), EngineThreadsRunning() counts
// both, and nothing is left running once the App has shut down.

#include <SDL3/SDL_timer.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>
#include <utility>

#include "platform/support/test_app.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_runtime/app.h"
#include "se_runtime/integration.h"
#include "support/test_roles.h"

namespace se {
namespace {

bool Live(ThreadRole role, std::string_view name) {
  const ThreadRegistry& registry = ThreadRegistry::Instance();
  for (std::uint32_t slot = 0; slot < kThreadSlotCount; ++slot) {
    const ThreadSlotInfo info = registry.Read(slot);
    if (info.state == ThreadSlotState::kLive && info.role == role &&
        info.Name() == name) {
      return true;
    }
  }
  return false;
}

// StartThread returns before the thread's RoleScope takes its slot: waits
// up to 2 s for it.
bool WaitLive(ThreadRole role, std::string_view name) {
  for (int i = 0; i < 2000 && !Live(role, name); ++i) SDL_DelayNS(1'000'000);
  return Live(role, name);
}

std::uint32_t LiveHelpers() {
  const ThreadRegistry& registry = ThreadRegistry::Instance();
  std::uint32_t live = 0;
  for (std::uint32_t slot = 0; slot < kThreadSlotCount; ++slot) {
    if (slot == kMainThreadSlot) continue;
    if (registry.Read(slot).state == ThreadSlotState::kLive) ++live;
  }
  return live;
}

TEST_CASE("The lane and the input sampler start in order and are counted",
          "[app][helper_threads]") {
  struct Seen {
    bool lane_at_boot = false;
    bool sampler_at_boot = true;
    bool log_worker_at_boot = false;
    bool watchdog_at_boot = false;
    std::uint32_t running_at_boot = 0;
    bool lane_running = false;
    bool sampler_running = false;
    std::uint32_t running = 0;
    std::uint32_t live = 0;
  } seen;
  GameSetup setup = testing::TestGameSetup();
  setup.on_boot = [&seen](App& /*app*/) {
    seen.lane_at_boot = WaitLive(ThreadRole::kBackground, "bg-lane");
    seen.log_worker_at_boot = WaitLive(ThreadRole::kLogWorker, "log-worker");
    seen.watchdog_at_boot = WaitLive(ThreadRole::kWatchdog, "watchdog");
    seen.sampler_at_boot = Live(ThreadRole::kInputSampler, "input-sampler");
    seen.running_at_boot = integration::EngineThreadsRunning();
  };
  setup.on_frame = [&seen](App& app, const FrameReport& report) {
    AssertMainThread();
    if (seen.sampler_running || report.iterate > 2000) return;
    // The sampler starts at the first iterate; its thread takes its slot
    // once it runs. A headless App does not wait for frames: 1 ms each.
    SDL_DelayNS(1'000'000);
    seen.lane_running = Live(ThreadRole::kBackground, "bg-lane");
    seen.sampler_running = Live(ThreadRole::kInputSampler, "input-sampler");
    seen.running = integration::EngineThreadsRunning();
    seen.live = LiveHelpers();
    if (seen.sampler_running || report.iterate == 2000) app.RequestQuit();
  };
  REQUIRE(testing::RunTestApp(std::move(setup), {"--headless"}) == 0);
  // Boot step 9 started the lane after the log worker and the watchdog;
  // the sampler waits for the first iterate.
  CHECK(seen.log_worker_at_boot);
  CHECK(seen.watchdog_at_boot);
  CHECK(seen.lane_at_boot);
  CHECK_FALSE(seen.sampler_at_boot);
  REQUIRE(seen.running_at_boot >= 3);
  // Both run while the App does, and EngineThreadsRunning counts them.
  CHECK(seen.lane_running);
  CHECK(seen.sampler_running);
  CHECK(seen.running == seen.running_at_boot + 1);
  CHECK(seen.running == seen.live);
  // Shut down: every engine thread has been joined.
  CHECK_FALSE(Live(ThreadRole::kBackground, "bg-lane"));
  CHECK_FALSE(Live(ThreadRole::kInputSampler, "input-sampler"));
  REQUIRE(integration::EngineThreadsRunning() == 0);
}

}  // namespace
}  // namespace se
