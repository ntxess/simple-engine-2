// Copyright 2026 The simple-engine-2 Authors.
//
// The App's assets, headless (M9; label unit, so the WSL sanitizer presets
// run it): the background lane starts with the helper threads and stops in
// the shutdown order, a state's World is held (no ticks planned) until its
// AssetScope is resident, and Durable saves through the lane keep a quit
// pending until they are renamed into place.

#include <SDL3/SDL_init.h>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/support/test_app.h"
#include "se_assets/asset_registry.h"
#include "se_assets/lane_jobs.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_platform/fs.h"
#include "se_platform/fs_faults.h"
#include "se_runtime/app.h"
#include "se_runtime/asset_runtime.h"
#include "se_runtime/integration.h"
#include "se_runtime/quit.h"
#include "support/test_roles.h"

#ifndef SE_TEST_OUT_DIR
#define SE_TEST_OUT_DIR "."
#endif

namespace se {
namespace {

using testing::RunTestApp;
using testing::TestGameSetup;

std::string Scratch(std::string_view name) {
  const std::string dir = fs::Join(SE_TEST_OUT_DIR, name);
  CHECK(fs::CreateDirectories(dir).has_value());
  return dir;
}

bool WriteText(const std::string& path, std::string_view text) {
  return fs::WriteFile(
             path,
             std::span<const std::byte>(
                 reinterpret_cast<const std::byte*>(text.data()), text.size()))
      .has_value();
}

TEST_CASE("A World is held until its AssetScope is resident", "[assets_app]") {
  if constexpr (!fs::kFsFaultHooks) {
    SKIP("the se::fs fault injector is compiled out of Release builds");
  }
  const std::string root = Scratch("assets_app_hold");
  REQUIRE(WriteText(fs::Join(root, "level.prefab.json"),
                    R"({"name": "level", "Transform": {"x": 1}})"));
  // A slow disk: every read step of the lane takes 60 ms.
  fs::SetFsFaults(fs::FsFaults{.read_step_delay_ms = 60});

  struct Seen {
    std::uint64_t held_frames = 0;
    std::uint64_t first_tick_iterate = 0;
    bool resident_at_first_tick = false;
    bool ticked_while_loading = false;
    bool lane_running = false;
    std::uint64_t first_iterate_ns = 0;
    std::uint64_t first_tick_ns = 0;
  } seen;
  GameSetup setup = TestGameSetup();
  setup.state_assets = [](std::string_view state) {
    StateAssets assets;
    if (state == "play") assets.prefabs.emplace_back("level.prefab.json");
    return assets;
  };
  setup.on_frame = [&seen](App& app, const FrameReport& report) {
    AssertMainThread();
    AssetRuntime* assets = app.Assets();
    REQUIRE(assets != nullptr);
    seen.lane_running = assets->Lane().Running();
    const AssetScope scope = assets->ScopeOf("play");
    const bool resident = assets->Registry().ScopeResident(scope);
    if (seen.first_iterate_ns == 0) seen.first_iterate_ns = report.now_ns;
    if (report.plan.steps > 0) {
      if (seen.first_tick_iterate == 0) {
        seen.first_tick_iterate = report.iterate;
        seen.first_tick_ns = report.now_ns;
        seen.resident_at_first_tick = resident;
      }
    } else if (seen.first_tick_iterate == 0) {
      ++seen.held_frames;
    }
    if (!resident && report.next_tick > report.first_tick) {
      seen.ticked_while_loading = true;
    }
    if (seen.first_tick_iterate != 0 &&
        report.iterate >= seen.first_tick_iterate + 10) {
      app.RequestQuit();
    }
    if (report.iterate > 200000) app.RequestQuit();
  };
  const int result = RunTestApp(
      std::move(setup),
      {"--headless", "--fixed-frame-dt", "16.6667", "--content-root", root});
  fs::ClearFsFaults();
  REQUIRE(result == 0);
  CHECK(seen.lane_running);
  REQUIRE(seen.first_tick_iterate > 1);
  REQUIRE(seen.held_frames > 0);
  CHECK(seen.resident_at_first_tick);
  CHECK_FALSE(seen.ticked_while_loading);
  // The prefab took at least its slow read step.
  REQUIRE(seen.first_tick_ns - seen.first_iterate_ns > 50'000'000ULL);
  // The lane stopped with the App: no engine thread is left.
  REQUIRE(integration::EngineThreadsRunning() == 0);
}

TEST_CASE("A Durable save through the lane keeps the quit pending",
          "[assets_app]") {
  if constexpr (!fs::kFsFaultHooks) {
    SKIP("the se::fs fault injector is compiled out of Release builds");
  }
  const std::string root = Scratch("assets_app_durable");
  const std::string path = fs::Join(root, "settings.json");
  REQUIRE(fs::Remove(path).has_value());
  // A throttled disk: three write steps of 80 ms.
  fs::SetFsFaults(fs::FsFaults{.write_step_delay_ms = 80});
  bool saved = false;
  std::uint64_t pending_frames = 0;
  std::uint64_t size_at_shutdown = 0;
  GameSetup setup = TestGameSetup();
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    if (!saved) {
      saved = true;
      SaveRequest save;
      save.path = path;
      save.revision = 1;
      save.durable = true;
      save.bytes.assign(std::size_t{3} << 20, std::byte{'x'});
      SubmitSave(app.Assets()->Lane(), std::move(save));
      app.RequestQuit();
      return;
    }
    if (app.Quit().Pending() && !app.Quit().ReadyToExit()) ++pending_frames;
    if (report.iterate > 400000) {
      app.Quit().RequestImmediate(QuitReason::kFault);
    }
  };
  // The quit's drain is over when SDL_AppQuit runs: the save is in place.
  setup.on_shutdown = [&](App& /*app*/, SDL_AppResult /*result*/) {
    if (auto info = fs::Stat(path)) size_at_shutdown = info->size;
  };
  const int result = RunTestApp(std::move(setup), {"--headless"});
  fs::ClearFsFaults();
  REQUIRE(result == 0);
  REQUIRE(pending_frames > 0);
  CHECK(size_at_shutdown == (std::uint64_t{3} << 20));
}

}  // namespace
}  // namespace se
