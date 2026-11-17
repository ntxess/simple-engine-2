// Copyright 2026 The simple-engine-2 Authors.
//
// The AssetRegistry's Text store (docs/milestone-specs.md M13a): a text
// file read on the lane in steps of at most 1 MB, handed to main as a
// shared immutable string, watched by the hot-reload scan (a reload
// replaces the string; holders of the old one keep it) and held back by
// the gameplay freeze like prefabs; a missing file fails with its path.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "assets/support/lane_test_support.h"
#include "se_assets/asset_registry.h"
#include "se_assets/background_lane.h"
#include "se_assets/lane_jobs.h"
#include "se_core/thread_role.h"
#include "se_platform/fs.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::MainThreadScope;
using testing::ScratchDir;
using testing::TextBytes;
using testing::WriteBytes;

void NoWake() noexcept {}

struct TextFixture {
  explicit TextFixture(std::uint64_t scan_ns) {
    LaneConfig config;
    config.wake_main = &NoWake;
    config.scan_period_ns = scan_ns;
    config.trace_capacity = 4096;
    auto started = BackgroundLane::Start(config);
    REQUIRE(started.has_value());
    lane = std::move(*started);
    AssetRegistryConfig registry_config;
    registry_config.hot_reload = true;
    registry = std::make_unique<AssetRegistry>(*lane, nullptr, registry_config);
  }

  template <class F>
  bool Frames(F done) SE_REQUIRES(g_main) {
    for (int i = 0; i < 5000; ++i) {
      registry->BeginFrame();
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  std::unique_ptr<BackgroundLane> lane;
  std::unique_ptr<AssetRegistry> registry;
};

std::string Big(std::size_t bytes, char fill) {
  std::string text = "# a scene padded to a few MB\n";
  text.append(bytes - text.size() - 1, fill);
  text += '\n';
  return text;
}

TEST_CASE("Text assets load in bounded steps and hot-reload",
          "[registry][hot_reload]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("registry_text");
  const std::string path = fs::Join(dir, "big.scene.json");
  const std::string first = Big((std::size_t{5} << 19) + 17, 'a');  // 2.5 MB
  REQUIRE(WriteBytes(path, TextBytes(first)));
  TextFixture f(/*scan_ns=*/50'000'000);
  const AssetScope scope = f.registry->CreateScope("gameplay");
  const Handle<TextAsset> text = f.registry->RequestText(scope, path);
  REQUIRE(f.registry->State(text) == AssetState::kLoading);
  REQUIRE(f.registry->Text(text) == nullptr);
  REQUIRE(f.Frames([&] { return f.registry->ScopeResident(scope); }));
  REQUIRE(f.registry->State(text) == AssetState::kResident);
  const std::shared_ptr<const std::string> loaded = f.registry->Text(text);
  REQUIRE(loaded != nullptr);
  REQUIRE(*loaded == first);
  REQUIRE(f.registry->Version(text) == 1);
  CHECK(f.registry->TextPath(text) == fs::Normalize(path));
  // The same file in another scope is the same entry.
  const AssetScope other = f.registry->CreateScope("pause");
  CHECK(f.registry->RequestText(other, path) == text);

  // Hot reload: the scan sees the change; the new text replaces the
  // registry's string, and the holder of the old one still has it.
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  f.registry->BeginFrame();
  const std::string second = Big(4096, 'b');
  REQUIRE(WriteBytes(path, TextBytes(second)));
  REQUIRE(f.Frames([&] { return f.registry->Version(text) == 2; }));
  REQUIRE(*f.registry->Text(text) == second);
  CHECK(*loaded == first);

  // Frozen (editor Play): a change waits for the end of the freeze.
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  f.registry->BeginFrame();
  f.registry->SetGameplayFrozen(true);
  const std::string third = Big(2048, 'c');
  REQUIRE(WriteBytes(path, TextBytes(third)));
  REQUIRE(f.Frames([&] { return f.registry->DeferredChanges() == 1; }));
  CHECK(*f.registry->Text(text) == second);
  f.registry->SetGameplayFrozen(false);
  REQUIRE(f.Frames([&] { return f.registry->Version(text) == 3; }));
  CHECK(*f.registry->Text(text) == third);

  // A missing file fails with its path.
  const Handle<TextAsset> missing =
      f.registry->RequestText(scope, fs::Join(dir, "missing.scene.json"));
  REQUIRE(f.Frames(
      [&] { return f.registry->State(missing) == AssetState::kFailed; }));
  REQUIRE(f.registry->LastError(missing) != nullptr);
  CHECK(f.registry->LastError(missing)->path.find("missing.scene.json") !=
        std::string::npos);
  f.registry->ReleaseAll();

  // Every step of the text jobs read at most 1 MB: the 2.5 MB file took an
  // opening step and three reads.
  f.lane->Stop();
  Ticket first_job = 0;
  std::size_t first_job_steps = 0;
  for (const LaneStepRecord& step : f.lane->StepTrace()) {
    if (std::string_view(step.kind.data()) != "text") continue;
    if (first_job == 0) first_job = step.ticket;
    if (step.ticket == first_job) ++first_job_steps;
  }
  const std::size_t reads =
      (first.size() + kReadStepBytes - 1) / kReadStepBytes;
  REQUIRE(reads == 3);
  CHECK(first_job_steps == 1 + reads);
}

}  // namespace
}  // namespace se
