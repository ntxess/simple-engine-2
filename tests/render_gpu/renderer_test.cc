// Copyright 2026 The simple-engine-2 Authors.
//
// Renderer2D offscreen (label gpu; once per driver, no window): the golden
// images of fixed scenes (tests/golden/render/<driver>/), buffer growth in
// the build phase, the frame transaction's faults (a failed step keeps the
// last image and the next frame draws), build-phase throws contained before
// anything is acquired, and device loss through ReleaseGpuObjects and
// RecreateGpuObjects.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "game_rampage/view.h"
#include "rampage/support/rampage_session.h"
#include "rampage/support/seinput.h"
#include "render_gpu/support/gpu_fixture.h"
#include "render_gpu/support/image_compare.h"
#include "se_core/detmath.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render/frame_faults.h"
#include "se_render/renderer_2d.h"
#include "se_render_core/render_list.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::CanvasCamera;
using testing::GpuFixture;
using testing::SceneBuilder;
using testing::SpriteOf;

constexpr float kDegrees = static_cast<float>(dm::kPi / 180.0);

std::unique_ptr<World> SpriteGrid() SE_REQUIRES(g_main) {
  SceneBuilder scene("sprite_grid");
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 5; ++column) {
      const auto shade = static_cast<std::uint8_t>(255 - 40 * row);
      scene.Add(SpriteOf("player", 48, 48, Rgba8{255, shade, 255, 255}),
                {32.0F + 48.0F * static_cast<float>(column),
                 32.0F + 48.0F * static_cast<float>(row)});
    }
  }
  return scene.Finish();
}

std::unique_ptr<World> RotatedSprites() SE_REQUIRES(g_main) {
  SceneBuilder scene("rotated");
  const float angles[] = {0, 30, 45, 90, 135, 180, 270, 333};
  for (int i = 0; i < 8; ++i) {
    SpriteRenderer sprite = SpriteOf("player", 48, 48);
    sprite.flip_x = i % 3 == 1;
    sprite.flip_y = i % 4 == 2;
    const float scale = 0.5F + 0.25F * static_cast<float>(i % 4);
    scene.Add(sprite,
              {40.0F + 60.0F * static_cast<float>(i % 4),
               50.0F + 90.0F * static_cast<float>(i >= 4 ? 1 : 0)},
              angles[i] * kDegrees, {scale, scale});
  }
  return scene.Finish();
}

std::unique_ptr<World> LayersAndAlpha() SE_REQUIRES(g_main) {
  SceneBuilder scene("layers_alpha");
  // A white floor, then checkers (half-transparent blue squares) over it on
  // higher layers, a translucent tint, and z ordering within a layer.
  SpriteRenderer floor = SpriteOf("white", 1, 1, Rgba8{40, 160, 40, 255});
  floor.layer = -1;
  scene.Add(floor, {128, 96}, 0.0F, {200, 120});
  for (int i = 0; i < 6; ++i) {
    SpriteRenderer checker = SpriteOf("checker", 16, 16);
    checker.layer = static_cast<std::int16_t>(i % 3);
    checker.z = static_cast<float>(5 - i);
    if (i == 4) checker.tint = Rgba8{255, 255, 255, 96};
    scene.Add(checker,
              {70.0F + 22.0F * static_cast<float>(i),
               60.0F + 12.0F * static_cast<float>(i)},
              0.0F, {4, 4});
  }
  return scene.Finish();
}

TexturePayload Render(Renderer2D& renderer, const World& world,
                      const Camera2D& camera) SE_REQUIRES(g_main) {
  RenderLayer layer;
  layer.world = &world;
  layer.camera = camera;
  const std::vector<RenderLayer> layers = {layer};
  return testing::RenderOffscreen(
      renderer, layers, static_cast<std::uint32_t>(camera.virtual_size.x),
      static_cast<std::uint32_t>(camera.virtual_size.y));
}

void CheckGolden(GpuFixture& gpu, const std::string& name,
                 const TexturePayload& image) {
  REQUIRE(image.width > 0);
  const testing::GoldenImageResult golden =
      testing::CheckGoldenImage(testing::DriverDir(gpu.Driver()), name, image);
  INFO(golden.message);
  CHECK(golden.ok);
}

TEST_CASE("Golden: a sprite grid", "[gpu][golden]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  REQUIRE(testing::LoadTestTextures(gpu.Resources()));
  const std::unique_ptr<World> world = SpriteGrid();
  CheckGolden(gpu, "sprite_grid",
              Render(gpu.Renderer(), *world, CanvasCamera(256, 192)));
}

TEST_CASE("Golden: rotated, scaled and flipped sprites", "[gpu][golden]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  REQUIRE(testing::LoadTestTextures(gpu.Resources()));
  const std::unique_ptr<World> world = RotatedSprites();
  CheckGolden(gpu, "rotated",
              Render(gpu.Renderer(), *world, CanvasCamera(256, 192)));
}

TEST_CASE("Golden: overlapping layers and alpha", "[gpu][golden]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  REQUIRE(testing::LoadTestTextures(gpu.Resources()));
  const std::unique_ptr<World> world = LayersAndAlpha();
  const TexturePayload image =
      Render(gpu.Renderer(), *world, CanvasCamera(256, 192));
  CheckGolden(gpu, "layers_alpha", image);
  // Straight alpha over the opaque floor stays opaque (BlendAlpha).
  REQUIRE(testing::PixelAt(image, 128, 96).a == 255);
}

TEST_CASE("Golden: the sandbox at tick 600", "[gpu][golden]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  REQUIRE(testing::LoadTestTextures(gpu.Resources()));
  // The sandbox scene with the legacy stress scenario's recorded input,
  // drawn at its tick-600 Transform (alpha 1).
  auto frames = testing::ReadSeinput(testing::RampageFixtureDir() +
                                     "/reference/stress.seinput");
  REQUIRE(frames);
  testing::FrameListInput input(std::move(*frames));
  testing::RampageSessionOptions options;
  options.scene = testing::SandboxSceneRequest();
  options.ticks = 600;
  options.input = &input;
  RenderList list;
  Camera2D camera;
  options.on_world = [&](const World& world) {
    if (world.Clock().tick != 600) return;
    camera = rampage::SandboxCamera(world);
    BuildRenderList(world, camera, 1.0F, list);
  };
  const testing::RampageSessionResult session =
      testing::RunRampageSession(options);
  REQUIRE(session.error.empty());
  REQUIRE(list.stats.quads > 0);
  RenderLayer layer;
  layer.list = &list;
  layer.camera = camera;
  const std::vector<RenderLayer> layers = {layer};
  const TexturePayload image = testing::RenderOffscreen(
      gpu.Renderer(), layers, static_cast<std::uint32_t>(camera.virtual_size.x),
      static_cast<std::uint32_t>(camera.virtual_size.y));
  REQUIRE(image.width == 1920);
  REQUIRE(image.height == 1080);
  CheckGolden(gpu, "sandbox_600", image);
}

TEST_CASE("The sprite buffers grow in the build phase and draw everything",
          "[gpu]") {
  const testing::MainThreadScope main;
  testing::GpuFixtureOptions options;
  options.renderer.initial_quads = 256;
  GpuFixture gpu(options);
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  REQUIRE(testing::LoadTestTextures(gpu.Resources()));
  Renderer2D& renderer = gpu.Renderer();
  REQUIRE(renderer.QuadCapacity() == 256);
  SceneBuilder scene("many");
  for (int row = 0; row < 30; ++row) {
    for (int column = 0; column < 100; ++column) {
      scene.Add(
          SpriteOf("white", 1, 1, Rgba8{255, 255, 255, 255}),
          {0.5F + static_cast<float>(column), 0.5F + static_cast<float>(row)});
    }
  }
  const std::unique_ptr<World> world = scene.Finish();
  const TexturePayload image = Render(renderer, *world, CanvasCamera(100, 40));
  REQUIRE(renderer.QuadCapacity() >= 3000);
  REQUIRE(renderer.Totals().vertex_growths >= 1);
  REQUIRE(renderer.LastFrame().quads == 3000);
  // Every one of the 30 rows is white; the last 10 rows are clear.
  CHECK(testing::PixelAt(image, 99, 29) == Rgba8{255, 255, 255, 255});
  CHECK(testing::PixelAt(image, 50, 35) == Rgba8{0, 0, 0, 255});
}

TEST_CASE(
    "A failed recording step keeps the last image; the next frame "
    "draws",
    "[gpu]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  REQUIRE(testing::LoadTestTextures(gpu.Resources()));
  Renderer2D& renderer = gpu.Renderer();
  const std::unique_ptr<World> grid = SpriteGrid();
  const std::unique_ptr<World> rotated = RotatedSprites();
  const Camera2D camera = CanvasCamera(256, 192);
  const TexturePayload grid_image = Render(renderer, *grid, camera);
  const TexturePayload rotated_image = Render(renderer, *rotated, camera);
  REQUIRE_FALSE(testing::CompareImages(grid_image, rotated_image).Matches());
  const std::uint64_t live = gpu.Resources().LiveObjects();

  for (const FrameStep step :
       {FrameStep::kUpload, FrameStep::kPrepare, FrameStep::kCopyPass,
        FrameStep::kViewportPass, FrameStep::kSubmit}) {
    INFO(FrameStepName(step));
    // Frame 1 draws the grid; frames 2-4 try the rotated scene with the
    // step failing; frame 5 draws it.
    renderer.Faults().Configure(FrameFaultConfig{});
    REQUIRE(testing::CompareImages(Render(renderer, *grid, camera), grid_image)
                .Matches());
    renderer.Faults().Configure(FrameFaultConfig{step, 1, 4});
    // An upload in flight, so that the upload step has work to fail.
    REQUIRE(gpu.Resources().QueueUpload(
        gpu.Resources().AddTexture("pending"),
        std::make_shared<const TexturePayload>(
            TexturePayload{2, 2, std::vector<std::uint8_t>(16, 7)})));
    for (std::uint64_t frame = 2; frame <= 4; ++frame) {
      renderer.Faults().SetFrame(frame);
      const TexturePayload image = Render(renderer, *rotated, camera);
      if (step == FrameStep::kCopyPass || step == FrameStep::kViewportPass) {
        // The composite keeps the grid.
        CHECK(testing::CompareImages(image, grid_image).Matches());
      }
    }
    REQUIRE(renderer.Faults().Injected(step) >= 3);
    renderer.Faults().SetFrame(5);
    CHECK(testing::CompareImages(Render(renderer, *rotated, camera),
                                 rotated_image)
              .Matches());
    renderer.Faults().Configure(FrameFaultConfig{});
    REQUIRE(gpu.Resources().FlushUploads());
  }
  // The "pending" slot's texture and a third fence in the renderer's ring
  // are new; nothing leaked.
  gpu.Device().WaitForIdle();
  CHECK(gpu.Resources().LiveObjects() <= live + 2);
}

TEST_CASE("A build-phase throw is contained before anything is acquired",
          "[gpu]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  REQUIRE(testing::LoadTestTextures(gpu.Resources()));
  Renderer2D& renderer = gpu.Renderer();
  const std::unique_ptr<World> grid = SpriteGrid();
  const Camera2D camera = CanvasCamera(256, 192);
  const TexturePayload expected = Render(renderer, *grid, camera);
  for (const FrameStep step : {FrameStep::kBuild, FrameStep::kVertices}) {
    INFO(FrameStepName(step));
    renderer.Faults().Configure(FrameFaultConfig{step, 1, 0});
    renderer.Faults().SetFrame(1);
    const std::uint64_t frames = gpu.Device().Stats().frames;
    RenderLayer layer;
    layer.world = grid.get();
    layer.camera = camera;
    const std::vector<RenderLayer> layers = {layer};
    FrameRequest request;
    request.layers = layers;
    request.present = false;
    const FrameResult result = renderer.Frame(request);
    REQUIRE(result.build_fault.has_value());
    CHECK(result.build_fault.value_or(Error{}).message.find(
              "render build fault") != std::string::npos);
    CHECK_FALSE(result.submitted);
    // No command buffer was acquired.
    CHECK(gpu.Device().Stats().frames == frames);
    renderer.Faults().Configure(FrameFaultConfig{});
    CHECK(testing::CompareImages(Render(renderer, *grid, camera), expected)
              .Matches());
  }
  REQUIRE(renderer.Totals().build_faults == 2);
}

TEST_CASE(
    "Device loss: the objects are recreated on a new device and the "
    "handles still resolve",
    "[gpu]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  REQUIRE(testing::LoadTestTextures(gpu.Resources()));
  const TextureHandle player = gpu.Resources().Find(TextureKeyOf("player"));
  REQUIRE_FALSE(player.IsNull());
  const std::unique_ptr<World> world = RotatedSprites();
  const Camera2D camera = CanvasCamera(256, 192);
  const TexturePayload before = Render(gpu.Renderer(), *world, camera);
  REQUIRE(gpu.RecreateDevice());
  CHECK(gpu.Resources().Find(TextureKeyOf("player")) == player);
  CHECK(gpu.Resources().IsResident(player));
  const TexturePayload after = Render(gpu.Renderer(), *world, camera);
  const testing::ImageDiff diff = testing::CompareImages(before, after);
  INFO(diff.Describe());
  CHECK(diff.Matches());
}

}  // namespace
}  // namespace se
