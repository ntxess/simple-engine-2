// Copyright 2026 The simple-engine-2 Authors.

#include "render_gpu/support/gpu_fixture.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_platform/sdl_context.h"
#include "se_platform/window.h"
#include "se_render/gpu_device.h"
#include "se_render/gpu_resources.h"
#include "se_render/image_io.h"
#include "se_render/renderer_2d.h"
#include "se_render_core/render_components.h"

#ifndef SE_SOURCE_DIR
#error "SE_SOURCE_DIR must name the repository root"
#endif

namespace se::testing {

std::string TestGpuDriver() {
  const char* driver = SDL_getenv("SE_GPU_DRIVER");
  return driver == nullptr || driver[0] == 0 ? std::string(DefaultGpuDriver())
                                             : std::string(driver);
}

std::string DriverDir(std::string_view driver) {
  if (driver == "direct3d12") return "d3d12";
  return std::string(driver);
}

std::string RepoRoot() { return SE_SOURCE_DIR; }

GpuFixture::GpuFixture(const GpuFixtureOptions& options)
    : driver_(TestGpuDriver()) {
  constexpr SDL_GPUShaderFormat kFormats =
      SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV;
  SdlInitConfig sdl_config;
  sdl_config.subsystems = SDL_INIT_VIDEO;
  auto sdl = SdlContext::Init(sdl_config);
  if (!sdl) {
    skip_reason_ = "SDL video: " + sdl.error().message;
    return;
  }
  sdl_.emplace(std::move(*sdl));
  if (!SDL_GPUSupportsShaderFormats(kFormats, driver_.c_str())) {
    skip_reason_ =
        "the GPU driver '" + driver_ + "' is not available on this machine";
    return;
  }
  if (options.window) {
    WindowConfig config;
    config.title = "se_render_gpu_tests";
    config.width = options.window_width;
    config.height = options.window_height;
    config.activate_when_shown = false;
    auto window = Window::Create(config);
    if (!window) {
      skip_reason_ = "no window: " + window.error().message;
      return;
    }
    window_ = std::move(*window);
  }
  GpuDeviceConfig config;
  config.driver = driver_;
  auto device = GpuDevice::Create(config);
  if (!device) {
    skip_reason_ = "GpuDevice::Create failed: " + device.error().message;
    return;
  }
  device_ = std::make_unique<GpuDevice>(std::move(*device));
  if (window_ != nullptr) {
    if (auto claimed = device_->ClaimWindow(window_->Native()); !claimed) {
      skip_reason_ = "ClaimWindow failed: " + claimed.error().message;
      return;
    }
  }
  auto renderer = Renderer2D::Create(*device_, options.renderer);
  if (!renderer) {
    skip_reason_ = "Renderer2D::Create failed: " + renderer.error().message;
    return;
  }
  renderer_ = std::move(*renderer);
}

GpuFixture::~GpuFixture() {
  renderer_.reset();
  device_.reset();
  window_.reset();
  sdl_.reset();
  SDL_Quit();
}

bool GpuFixture::RecreateDevice() {
  renderer_->ReleaseGpuObjects();
  SDL_Window* window = device_->ClaimedWindow();
  device_.reset();
  GpuDeviceConfig config;
  config.driver = driver_;
  auto device = GpuDevice::Create(config);
  if (!device) return false;
  device_ = std::make_unique<GpuDevice>(std::move(*device));
  if (window != nullptr && !device_->ClaimWindow(window)) return false;
  return renderer_->RecreateGpuObjects(*device_).has_value();
}

const ComponentRegistry& SceneRegistry() {
  static const ComponentRegistry kRegistry = [] {
    ComponentRegistry registry;
    RegisterCoreComponents(registry);
    RegisterRenderComponents(registry);
    registry.Seal();
    return registry;
  }();
  return kRegistry;
}

SceneBuilder::SceneBuilder(std::string_view name) {
  WorldDesc desc;
  desc.name = std::string(name);
  world_ = std::make_unique<World>(SceneRegistry(), std::move(desc));
}

SceneBuilder& SceneBuilder::Add(const SpriteRenderer& sprite, Vec2 pos,
                                float rot, Vec2 scale) {
  const InlineOwnerScope owner(OnMainThread());
  const entt::entity entity = world_->Create(Guid{0x5CE0, next_++});
  Transform transform;
  transform.pos = pos;
  transform.rot = rot;
  transform.scale = scale;
  world_->Reg().emplace<Transform>(entity, transform);
  world_->Reg().emplace<SpriteRenderer>(entity, sprite);
  return *this;
}

std::unique_ptr<World> SceneBuilder::Finish() {
  const InlineOwnerScope owner(OnMainThread());
  world_->FinalizeSpawns();
  return std::move(world_);
}

SpriteRenderer SpriteOf(std::string_view name, float w, float h, Rgba8 tint) {
  SpriteRenderer sprite;
  sprite.texture = TextureKeyOf(name);
  sprite.region = {0.0F, 0.0F, w, h};
  sprite.origin = {0.5F * w, 0.5F * h};
  sprite.tint = tint;
  return sprite;
}

Camera2D CanvasCamera(float w, float h) {
  Camera2D camera;
  camera.center = {0.5F * w, 0.5F * h};
  camera.zoom = 1.0F;
  camera.virtual_size = {w, h};
  camera.primary = true;
  return camera;
}

bool LoadTestTextures(GpuResources& resources) {
  auto player = LoadPng(RepoRoot() + "/assets/player/player.png");
  if (!player) return false;
  if (!resources.QueueUpload(
          resources.AddTexture("player"),
          std::make_shared<const TexturePayload>(std::move(*player)))) {
    return false;
  }
  auto checker = std::make_shared<TexturePayload>();
  checker->width = 16;
  checker->height = 16;
  checker->pixels.resize(checker->Bytes());
  for (std::uint32_t y = 0; y < 16; ++y) {
    for (std::uint32_t x = 0; x < 16; ++x) {
      const bool red = ((x / 4) + (y / 4)) % 2 == 0;
      std::uint8_t* p = &checker->pixels[(std::size_t{y} * 16 + x) * 4];
      p[0] = red ? 255 : 0;
      p[1] = 0;
      p[2] = red ? 0 : 255;
      p[3] = red ? 255 : 128;
    }
  }
  if (!resources.QueueUpload(resources.AddTexture("checker"), checker)) {
    return false;
  }
  auto white = std::make_shared<TexturePayload>();
  white->width = 1;
  white->height = 1;
  white->pixels = {255, 255, 255, 255};
  if (!resources.QueueUpload(resources.AddTexture("white"), white)) {
    return false;
  }
  return resources.FlushUploads().has_value();
}

TexturePayload RenderOffscreen(Renderer2D& renderer,
                               std::span<const RenderLayer> layers,
                               std::uint32_t width, std::uint32_t height) {
  renderer.SetCompositeSize(width, height);
  FrameRequest request;
  request.layers = layers;
  request.clear = ClearColor{0.0F, 0.0F, 0.0F, 1.0F};
  request.present = false;
  const FrameResult result = renderer.Frame(request);
  if (!result.submitted) return {};
  auto image = renderer.Readback();
  return image ? std::move(*image) : TexturePayload{};
}

Rgba8 PixelAt(const TexturePayload& image, std::uint32_t x, std::uint32_t y) {
  if (x >= image.width || y >= image.height) return Rgba8{0, 0, 0, 0};
  const std::uint8_t* p = &image.pixels[(std::size_t{y} * image.width + x) * 4];
  return Rgba8{p[0], p[1], p[2], p[3]};
}

}  // namespace se::testing
