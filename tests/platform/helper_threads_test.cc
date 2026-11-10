// Copyright 2026 The simple-engine-2 Authors.
//
// The helper threads in the App's boot and shutdown order (docs/milestone-
// specs.md "Integration (M8 + M9 + M17a)"; ADR-088), headless (label unit,
// so the WSL sanitizer presets run it): the background lane is a live
// ThreadRegistry slot when the boot ends, the input sampler joins it at the
// first iterate (inside ADR-047's window), EngineThreadsRunning() counts
// both, and nothing is left running once the App has shut down. M12: the
// Play-start bytes hook registers and withdraws its dump stream.

#include <SDL3/SDL_timer.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/support/test_app.h"
#include "se_core/dump_stream.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_crash/crash.h"
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

std::vector<std::byte> BytesOf(std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  return bytes;
}

TEST_CASE("The Play-start bytes are a dump stream while they are attached",
          "[app][helper_threads]") {
  const std::vector<std::byte> first = BytesOf(R"({"format": "se-scene-v1"})");
  const std::vector<std::byte> second = BytesOf("the second Play");
  integration::PlayStartStream play;
  REQUIRE_FALSE(play.Attached());
  REQUIRE(play.Attach(first));
  REQUIRE(play.Attached());
  const auto region = play.StreamBytes();
  REQUIRE(region.size() == sizeof(DumpStreamHeader) + first.size());
  DumpStreamHeader header;
  std::memcpy(&header, region.data(), sizeof(header));
  CHECK(header.magic == kDumpStreamMagic);
  CHECK(header.kind == static_cast<std::uint16_t>(DumpStreamKind::kBytes));
  REQUIRE(header.record_size == 1);
  CHECK(header.capacity == first.size());
  CHECK(header.head == first.size());
  REQUIRE(std::string_view(header.name) == "play-start");
  REQUIRE(std::memcmp(region.data() + sizeof(header), first.data(),
                      first.size()) == 0);
  // The stream type is taken while the bytes are attached.
  static std::array<std::byte, 8> other{};
  CHECK_FALSE(crash::RegisterStream(crash::StreamType::kPlayStartBytes,
                                    other.data(), other.size()));
  // The next Play replaces them.
  REQUIRE(play.Attach(second));
  CHECK(play.StreamBytes().size() == sizeof(DumpStreamHeader) + second.size());
  // Stop withdraws the stream: the type is free again.
  play.Detach();
  REQUIRE_FALSE(play.Attached());
  CHECK(play.StreamBytes().empty());
  play.Detach();
  REQUIRE(crash::RegisterStream(crash::StreamType::kPlayStartBytes,
                                other.data(), other.size()));
  REQUIRE(crash::UnregisterStream(crash::StreamType::kPlayStartBytes));
  CHECK_FALSE(play.Attach({}));
  // The destructor detaches.
  {
    integration::PlayStartStream scoped;
    REQUIRE(scoped.Attach(first));
  }
  REQUIRE(crash::RegisterStream(crash::StreamType::kPlayStartBytes,
                                other.data(), other.size()));
  REQUIRE(crash::UnregisterStream(crash::StreamType::kPlayStartBytes));
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
