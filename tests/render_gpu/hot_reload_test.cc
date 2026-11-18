// Copyright 2026 The simple-engine-2 Authors.
//
// Hot reload while rampage runs (docs/milestone-specs.md M9; label
// desktop, once per driver): rampage's textures load from cooked pages
// through the background lane; a texture file changed on disk is seen by
// the lane's 500 ms scan, re-cooked on the lane (Urgent), uploaded, and
// swapped at a frame start once resident. The readback shows the new
// pixels, and the old texture is released right after the swap frame's
// submit (GpuResources' live texture count returns to where it was).
// The window opens without focus and nothing touches the real input.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/module.h"
#include "game_rampage/view.h"
#include "platform/support/test_app.h"
#include "render_gpu/support/gpu_fixture.h"
#include "render_gpu/support/image_compare.h"
#include "se_assets/asset_registry.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_platform/fs.h"
#include "se_render/gpu_resources.h"
#include "se_render/image_io.h"
#include "se_render/renderer_2d.h"
#include "se_render_core/render_components.h"
#include "se_runtime/app.h"
#include "se_runtime/asset_runtime.h"
#include "support/test_roles.h"

namespace se {
namespace {

constexpr Rgba8 kGreen{0, 255, 0, 255};
constexpr Rgba8 kBlack{0, 0, 0, 255};

// Copies the files rampage's sandbox reads into |root|.
void CopyContent(const std::string& repo, const std::string& root) {
  for (const char* file :
       {"config/texture.json", "assets/scenes/sandbox.scene.json",
        "assets/prefabs/player.prefab.json", "assets/prefabs/enemy.prefab.json",
        "assets/player/player.png", "assets/coin/coin.png",
        "assets/bg_city/city 1/1.png"}) {
    const std::string to = fs::Join(root, file);
    REQUIRE(fs::CreateDirectories(fs::Parent(to)).has_value());
    auto bytes = fs::ReadFile(fs::Join(repo, file));
    REQUIRE(bytes.has_value());
    REQUIRE(fs::WriteFile(to, *bytes).has_value());
  }
}

void Count(const TexturePayload& image, std::uint64_t& green,
           std::uint64_t& other) {
  green = 0;
  other = 0;
  for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
    const Rgba8 pixel{image.pixels[i], image.pixels[i + 1], image.pixels[i + 2],
                      image.pixels[i + 3]};
    if (pixel == kGreen) {
      ++green;
    } else if (pixel != kBlack) {
      ++other;
    }
  }
}

TEST_CASE("A texture changed on disk while rampage runs is hot-reloaded",
          "[desktop]") {
  const testing::MainThreadScope main;
  const std::string root = testing::ImageOutDir() + "/hot_reload_content";
  CopyContent(testing::RepoRoot(), root);
  const std::string player_png = fs::Join(root, "assets/player/player.png");
  std::vector<std::byte> green_png;
  {
    TexturePayload green;
    green.width = 48;
    green.height = 48;
    green.pixels.resize(green.Bytes());
    for (std::size_t i = 0; i < green.pixels.size(); i += 4) {
      green.pixels[i + 1] = 255;
      green.pixels[i + 3] = 255;
    }
    auto encoded = EncodePng(green.width, green.height, green.pixels);
    REQUIRE(encoded.has_value());
    green_png = std::move(*encoded);
  }

  GameSetup setup;
  setup.module = &rampage::RampageModule();
  // The sandbox (M13b: rampage's session starts at the Title).
  setup.start_state = std::string(rampage::kSandboxState);
  setup.title = "se_render_gpu_tests hot reload";
  setup.org = "simple-engine-tests";
  setup.app = "render_gpu";
  setup.window_width = 480;
  setup.window_height = 270;
  setup.clear_color = ClearColor{0.0F, 0.0F, 0.0F, 1.0F};
  setup.texture_table = "config/texture.json";
  setup.camera = [](const World& world) {
    return rampage::SandboxCamera(world);
  };
  setup.configure =
      [](const ContentPaths& paths) -> std::expected<void, Error> {
    rampage::SceneRequest request;
    request.path =
        fs::Join(paths.content_root, "assets/scenes/sandbox.scene.json");
    request.asset_root = fs::Join(paths.content_root, "assets");
    request.legacy_root = paths.content_root;
    rampage::SetSandboxScene(std::move(request));
    return {};
  };

  enum class Phase { kWarm, kWaitSwap, kAfterSwap, kDone };
  Phase phase = Phase::kWarm;
  std::uint64_t presented = 0;
  std::uint64_t change_iterate = 0;
  std::uint32_t generation_before = 0;
  std::uint64_t live_before = 0;
  std::uint64_t live_max_during = 0;
  std::uint64_t live_after_swap = 0;
  std::size_t retire_after_swap = 1;
  std::uint64_t green_before = 1;
  std::uint64_t other_before = 0;
  std::uint64_t green_after = 0;
  std::uint64_t other_after = 1;
  bool from_page = false;
  bool recooked = false;
  RegistryStats stats;
  std::string failure;

  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    Renderer2D* renderer = app.Renderer();
    AssetRuntime* assets = app.Assets();
    if (renderer == nullptr || assets == nullptr) {
      failure = "no renderer or no assets";
      app.RequestQuit();
      return;
    }
    GpuResources& resources = renderer->Resources();
    AssetRegistry& registry = assets->Registry();
    const Handle<TextureAsset> player = registry.FindTexture("player");
    const TextureHandle slot = registry.GpuTexture(player);
    if (report.presented) ++presented;
    const std::uint64_t live = resources.LiveObjects(GpuObjectKind::kTexture);
    switch (phase) {
      case Phase::kWarm:
        if (presented >= 10 &&
            registry.State(player) == AssetState::kResident) {
          const AtlasInfo* atlas = registry.Atlas(registry.AtlasOf(player));
          from_page = atlas != nullptr && !atlas->recooked;
          auto image = renderer->Readback();
          if (!image) {
            failure = image.error().message;
            app.RequestQuit();
            return;
          }
          Count(*image, green_before, other_before);
          generation_before = resources.ContentGeneration(slot);
          live_before = live;
          live_max_during = live;
          // An editor saving the texture: a new file renamed over it.
          if (auto written = fs::WriteFileAtomic(player_png, green_png);
              !written) {
            failure = written.error().message;
            app.RequestQuit();
            return;
          }
          change_iterate = report.iterate;
          phase = Phase::kWaitSwap;
        }
        break;
      case Phase::kWaitSwap:
        live_max_during = std::max(live_max_during, live);
        if (resources.ContentGeneration(slot) != generation_before) {
          // Swapped at this frame's start; the old texture was released
          // right after this frame's submit.
          live_after_swap = live;
          retire_after_swap = resources.RetireQueueSize();
          const AtlasInfo* atlas = registry.Atlas(registry.AtlasOf(player));
          recooked = atlas != nullptr && atlas->recooked;
          phase = Phase::kAfterSwap;
        }
        break;
      case Phase::kAfterSwap:
        if (report.presented) {
          auto image = renderer->Readback();
          if (!image) {
            failure = image.error().message;
          } else {
            Count(*image, green_after, other_after);
            if (auto written = WritePng(
                    testing::ImageOutDir() + "/hot_reload.png", *image);
                !written) {
              failure = written.error().message;
            }
          }
          stats = registry.Stats();
          phase = Phase::kDone;
          app.RequestQuit();
        }
        break;
      case Phase::kDone:
        break;
    }
    // About 10 s at 165 Hz: the scan runs every 500 ms.
    if (report.iterate > 2000 &&
        (phase == Phase::kWarm || report.iterate > change_iterate + 1700)) {
      failure = "the test did not finish";
      app.RequestQuit();
    }
  };
  REQUIRE(testing::RunTestApp(std::move(setup), {"--content-root", root,
                                                 "--hot-reload", "on"}) == 0);
  INFO(failure);
  CHECK(failure.empty());
  REQUIRE(phase == Phase::kDone);
  // The first texture came from its cooked page, the reload from a
  // re-cook of the changed PNG on the lane.
  CHECK(from_page);
  CHECK(recooked);
  REQUIRE(stats.file_changes >= 1);
  REQUIRE(stats.reloads_resident == 1);
  // Before: no pure green. After: every sprite pixel is the new green.
  REQUIRE(green_before == 0);
  REQUIRE(other_before > 100);
  REQUIRE(green_after > 1000);
  REQUIRE(other_after == 0);
  // The new texture lived beside the old one while it uploaded; the old
  // one went through the retire queue after the swap frame's submit.
  REQUIRE(live_max_during == live_before + 1);
  REQUIRE(live_after_swap == live_before);
  REQUIRE(retire_after_swap == 0);
}

}  // namespace
}  // namespace se
