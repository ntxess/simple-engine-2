// Copyright 2026 The simple-engine-2 Authors.
//
// Renderer2D on a window (label desktop; once per driver): two viewport
// targets and the composite in one frame, and the M7 exit test of a
// texture reloaded while the App's window is minimized. Windows open
// without taking focus; minimize and restore use the non-activating forms
// (ShowWindow SW_SHOWMINNOACTIVE / SW_SHOWNOACTIVATE, as M5's tests do):
// SDL_MinimizeWindow activates the next window and SDL_RestoreWindow
// activates ours, which would take the focus from whoever uses the
// machine.

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "game_rampage/module.h"
#include "game_rampage/view.h"
#include "platform/support/test_app.h"
#include "render_gpu/support/gpu_fixture.h"
#include "render_gpu/support/image_compare.h"
#include "se_core/error.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_platform/fs.h"
#include "se_render/image_io.h"
#include "se_render/renderer_2d.h"
#include "se_render/texture_table.h"
#include "se_render_core/render_components.h"
#include "se_runtime/app.h"
#include "support/test_roles.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace se {
namespace {

using testing::CanvasCamera;
using testing::GpuFixture;
using testing::SceneBuilder;
using testing::SpriteOf;

constexpr Rgba8 kRed{255, 0, 0, 255};
constexpr Rgba8 kBlue{0, 0, 255, 255};
constexpr Rgba8 kGreen{0, 255, 0, 255};

void MinimizeWithoutActivating(SDL_Window* window) {
#if defined(_WIN32)
  auto* hwnd = static_cast<HWND>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
#else
  SDL_MinimizeWindow(window);
#endif
}

void RestoreWithoutActivating(SDL_Window* window) {
#if defined(_WIN32)
  auto* hwnd = static_cast<HWND>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
#else
  SDL_RestoreWindow(window);
#endif
}

TEST_CASE("Two viewports and the composite in one frame", "[desktop]") {
  const testing::MainThreadScope main;
  testing::GpuFixtureOptions options;
  options.window = true;
  options.window_width = 320;
  options.window_height = 240;
  GpuFixture gpu(options);
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  REQUIRE(testing::LoadTestTextures(gpu.Resources()));
  Renderer2D& renderer = gpu.Renderer();

  // A red square at (50, 50) and a blue one at (150, 100), 20 px each.
  SceneBuilder scene("viewports");
  scene.Add(SpriteOf("white", 1, 1, kRed), {50, 50}, 0.0F, {20, 20});
  scene.Add(SpriteOf("white", 1, 1, kBlue), {150, 100}, 0.0F, {20, 20});
  const std::unique_ptr<World> world = scene.Finish();

  // Viewport 1 looks at the red square at zoom 4; viewport 2 at the blue
  // one at zoom 2; the composite shows the whole 200 x 150 world.
  auto first = renderer.CreateViewport(64, 64);
  auto second = renderer.CreateViewport(96, 48);
  REQUIRE(first);
  REQUIRE(second);
  Camera2D red_camera = CanvasCamera(64, 64);
  red_camera.center = {50, 50};
  red_camera.zoom = 4.0F;
  (*first)->SetCamera(red_camera);
  Camera2D blue_camera = CanvasCamera(96, 48);
  blue_camera.center = {150, 100};
  blue_camera.zoom = 2.0F;
  (*second)->SetCamera(blue_camera);

  RenderLayer layer;
  layer.world = world.get();
  layer.camera = CanvasCamera(200, 150);
  const std::vector<RenderLayer> layers = {layer};
  std::vector<ViewportDraw> viewports = {{*first, layers}, {*second, layers}};
  FrameRequest request;
  request.layers = layers;
  request.viewports = viewports;
  request.clear = ClearColor{0.0F, 0.0F, 0.0F, 1.0F};
  SDL_PumpEvents();
  FrameResult result = renderer.Frame(request);
  REQUIRE(result.submitted);
  REQUIRE(renderer.LastFrame().targets == 3);
  REQUIRE(renderer.LastFrame().draws == 3);

  auto red_view = renderer.Readback(*first);
  auto blue_view = renderer.Readback(*second);
  auto composite = renderer.Readback();
  REQUIRE(red_view);
  REQUIRE(blue_view);
  REQUIRE(composite);
  REQUIRE(red_view->width == 64);
  // Zoom 4: the 20 px square covers 80 px, the whole 64 px viewport.
  CHECK(testing::PixelAt(*red_view, 1, 1) == kRed);
  CHECK(testing::PixelAt(*red_view, 62, 62) == kRed);
  // Zoom 2: 40 px of blue in the middle of 96 x 48, black at the sides.
  CHECK(testing::PixelAt(*blue_view, 48, 24) == kBlue);
  CHECK(testing::PixelAt(*blue_view, 5, 24) == Rgba8{0, 0, 0, 255});
  // The composite is the window's 320 x 240: the 200 x 150 canvas at 1.6.
  REQUIRE(composite->width == 320);
  REQUIRE(composite->height == 240);
  CHECK(testing::PixelAt(*composite, 80, 80) == kRed);
  CHECK(testing::PixelAt(*composite, 240, 160) == kBlue);
  CHECK(testing::PixelAt(*composite, 160, 120) == Rgba8{0, 0, 0, 255});

  // A resize applies at the next frame start; the old texture is retired.
  const std::uint64_t retired = gpu.Resources().Stats().retired;
  (*first)->RequestSize(32, 32);
  REQUIRE((*first)->Width() == 64);
  SDL_PumpEvents();
  result = renderer.Frame(request);
  REQUIRE(result.submitted);
  REQUIRE((*first)->Width() == 32);
  CHECK(gpu.Resources().Stats().retired > retired);
  auto smaller = renderer.Readback(*first);
  REQUIRE(smaller);
  REQUIRE(smaller->width == 32);
  CHECK(testing::PixelAt(*smaller, 16, 16) == kRed);
  renderer.DestroyViewport(*first);
  renderer.DestroyViewport(*second);
}

// HalfRate's re-present (wired at the M6 integration): the blit of the last
// composite again, without a RenderList, vertices or an offscreen pass.
TEST_CASE("RepresentLast blits the last composite again", "[desktop]") {
  const testing::MainThreadScope main;
  testing::GpuFixtureOptions options;
  options.window = true;
  options.window_width = 320;
  options.window_height = 240;
  GpuFixture gpu(options);
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  REQUIRE(testing::LoadTestTextures(gpu.Resources()));
  Renderer2D& renderer = gpu.Renderer();

  // Before any frame the composite was never drawn: the swapchain is
  // cleared and still presented.
  SDL_PumpEvents();
  const FrameResult first = renderer.RepresentLast();
  CHECK(first.submitted);
  CHECK_FALSE(first.error.has_value());

  SceneBuilder scene("represent");
  scene.Add(SpriteOf("white", 1, 1, kRed), {100, 75}, 0.0F, {40, 40});
  const std::unique_ptr<World> world = scene.Finish();
  RenderLayer layer;
  layer.world = world.get();
  layer.camera = CanvasCamera(200, 150);
  const std::vector<RenderLayer> layers = {layer};
  FrameRequest request;
  request.layers = layers;
  request.clear = ClearColor{0.0F, 0.0F, 0.0F, 1.0F};
  SDL_PumpEvents();
  REQUIRE(renderer.Frame(request).submitted);
  const std::uint64_t frames = renderer.Totals().frames;
  const std::uint64_t device_frames = gpu.Device().Stats().frames;
  auto drawn = renderer.Readback();
  REQUIRE(drawn);
  REQUIRE(testing::PixelAt(*drawn, 160, 120) == kRed);

  constexpr int kRepresents = 4;
  int presented = 0;
  for (int i = 0; i < kRepresents; ++i) {
    SDL_PumpEvents();
    const FrameResult result = renderer.RepresentLast();
    CHECK(result.submitted);
    CHECK_FALSE(result.build_fault.has_value());
    CHECK_FALSE(result.error.has_value());
    if (result.presented) ++presented;
  }
  // One command buffer each, no new frame of the renderer; the swapchain
  // wait makes every acquire succeed on a visible window, but a NULL
  // acquire is allowed (it is not a state change).
  CHECK(renderer.Totals().represents == kRepresents + 1);
  CHECK(renderer.Totals().frames == frames);
  CHECK(gpu.Device().Stats().frames == device_frames + kRepresents);
  CHECK(presented + static_cast<int>(renderer.Totals().null_acquires) >=
        kRepresents);
  REQUIRE(presented > 0);
  // The composite is untouched: the re-present only reads it.
  auto again = renderer.Readback();
  REQUIRE(again);
  REQUIRE(again->width == drawn->width);
  REQUIRE(again->height == drawn->height);
  CHECK(again->pixels == drawn->pixels);
}

// The sandbox in the real App, the player texture replaced by an opaque
// green one while the window is minimized.
TEST_CASE("A texture reloaded while minimized is correct after restore",
          "[desktop]") {
  const testing::MainThreadScope main;
  const std::string replacement = testing::ImageOutDir() + "/green48.png";
  {
    TexturePayload green;
    green.width = 48;
    green.height = 48;
    green.pixels.resize(green.Bytes());
    for (std::size_t i = 0; i < green.pixels.size(); i += 4) {
      green.pixels[i + 1] = 255;
      green.pixels[i + 3] = 255;
    }
    REQUIRE(WritePng(replacement, green));
  }

  GameSetup setup;
  setup.module = &rampage::RampageModule();
  setup.title = "se_render_gpu_tests minimized reload";
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

  enum class Phase { kShow, kMinimize, kWaitResident, kRestore, kDone };
  Phase phase = Phase::kShow;
  std::uint64_t presented = 0;
  std::uint64_t phase_start = 0;
  std::uint32_t generation_before = 0;
  bool reloaded_while_minimized = false;
  bool uploaded_while_minimized = false;
  std::uint64_t green_before = 1;
  std::uint64_t green_after = 0;
  std::uint64_t other_after = 0;
  std::string failure;

  auto count = [](const TexturePayload& image, std::uint64_t& green,
                  std::uint64_t& other) {
    green = 0;
    other = 0;
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
      const Rgba8 pixel{image.pixels[i], image.pixels[i + 1],
                        image.pixels[i + 2], image.pixels[i + 3]};
      if (pixel == kGreen) {
        ++green;
      } else if (pixel != Rgba8{0, 0, 0, 255}) {
        ++other;
      }
    }
  };

  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    Renderer2D* renderer = app.Renderer();
    if (renderer == nullptr) {
      failure = "no renderer";
      app.RequestQuit();
      return;
    }
    GpuResources& resources = renderer->Resources();
    const TextureHandle player = resources.Find(TextureKeyOf("player"));
    if (report.presented) ++presented;
    switch (phase) {
      case Phase::kShow:
        if (presented >= 10) {
          auto image = renderer->Readback();
          if (image) {
            std::uint64_t other = 0;
            count(*image, green_before, other);
          }
          MinimizeWithoutActivating(app.GetWindow()->Native());
          phase = Phase::kMinimize;
          phase_start = report.iterate;
        }
        break;
      case Phase::kMinimize:
        if (report.suspended) {
          generation_before = resources.ContentGeneration(player);
          if (auto loaded = LoadTextureFile(resources, player, replacement);
              !loaded) {
            failure = loaded.error().message;
            app.RequestQuit();
          }
          reloaded_while_minimized = true;
          phase = Phase::kWaitResident;
          phase_start = report.iterate;
        }
        break;
      case Phase::kWaitResident:
        // The uploads run on iterates that render nothing (10 Hz while
        // Suspended).
        if (report.suspended && !report.rendered &&
            resources.ContentGeneration(player) > generation_before) {
          uploaded_while_minimized = true;
        }
        if (uploaded_while_minimized || report.iterate > phase_start + 100) {
          RestoreWithoutActivating(app.GetWindow()->Native());
          phase = Phase::kRestore;
          phase_start = report.iterate;
        }
        break;
      case Phase::kRestore:
        if (!report.suspended && report.presented &&
            report.iterate > phase_start + 2) {
          auto image = renderer->Readback();
          if (!image) {
            failure = image.error().message;
          } else {
            count(*image, green_after, other_after);
            if (auto written = WritePng(
                    testing::ImageOutDir() + "/minimized_reload.png", *image);
                !written) {
              failure = written.error().message;
            }
          }
          phase = Phase::kDone;
          app.RequestQuit();
        }
        break;
      case Phase::kDone:
        break;
    }
    if (report.iterate > 3000) {
      failure = "the test did not finish";
      app.RequestQuit();
    }
  };
  REQUIRE(testing::RunTestApp(std::move(setup),
                              {"--content-root", testing::RepoRoot()}) == 0);
  INFO(failure);
  CHECK(failure.empty());
  CHECK(phase == Phase::kDone);
  CHECK(reloaded_while_minimized);
  CHECK(uploaded_while_minimized);
  // Before: no pure green. After: every sprite pixel is the replacement's
  // green (nearest sampling of an opaque texture), nothing of the old one.
  REQUIRE(green_before == 0);
  REQUIRE(green_after > 1000);
  REQUIRE(other_after == 0);
}

}  // namespace
}  // namespace se
