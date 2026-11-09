// Copyright 2026 The simple-engine-2 Authors.
//
// GPU test support: a device without a window (label gpu) or with one
// (label desktop) and the renderer on it, test scenes and textures. CTest
// registers every [gpu] and [desktop] test once per driver with
// SE_GPU_DRIVER set (direct3d12, vulkan); a driver the machine cannot run
// skips the test with the reason (SDL_GPUSupportsShaderFormats), any other
// failure fails it.

#ifndef TESTS_RENDER_GPU_SUPPORT_GPU_FIXTURE_H_
#define TESTS_RENDER_GPU_SUPPORT_GPU_FIXTURE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "se_core/component_registry.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_platform/sdl_context.h"
#include "se_platform/window.h"
#include "se_render/gpu_device.h"
#include "se_render/gpu_resources.h"
#include "se_render/renderer_2d.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"

namespace se::testing {

// SE_GPU_DRIVER, else the platform's default driver.
[[nodiscard]] std::string TestGpuDriver();
// The golden directory name of |driver|: d3d12 or vulkan.
[[nodiscard]] std::string DriverDir(std::string_view driver);

struct GpuFixtureOptions {
  // Open a window (shown without focus) and claim it.
  bool window = false;
  int window_width = 320;
  int window_height = 240;
  Renderer2DConfig renderer;
};

class GpuFixture {
 public:
  explicit GpuFixture(const GpuFixtureOptions& options = {})
      SE_REQUIRES(g_main);
  GpuFixture(const GpuFixture&) = delete;
  GpuFixture& operator=(const GpuFixture&) = delete;
  ~GpuFixture();

  // False when the driver is not available here; SkipReason() says why.
  [[nodiscard]] bool Available() const noexcept { return renderer_ != nullptr; }
  [[nodiscard]] const std::string& SkipReason() const noexcept {
    return skip_reason_;
  }
  [[nodiscard]] const std::string& Driver() const noexcept { return driver_; }
  [[nodiscard]] GpuDevice& Device() noexcept { return *device_; }
  [[nodiscard]] Renderer2D& Renderer() noexcept { return *renderer_; }
  [[nodiscard]] GpuResources& Resources() noexcept {
    return renderer_->Resources();
  }
  [[nodiscard]] Window* GetWindow() noexcept { return window_.get(); }
  // Destroys the renderer's device and makes a new one (device loss).
  [[nodiscard]] bool RecreateDevice() SE_REQUIRES(g_main);

 private:
  std::string driver_;
  std::string skip_reason_;
  std::optional<SdlContext> sdl_;
  std::unique_ptr<Window> window_;
  std::unique_ptr<GpuDevice> device_;
  std::unique_ptr<Renderer2D> renderer_;
};

// --- Scenes ---

// The core and render components, sealed.
[[nodiscard]] const ComponentRegistry& SceneRegistry();

// A World with one sprite per Add call, all spawned (FinalizeSpawns runs in
// Finish).
class SceneBuilder {
 public:
  explicit SceneBuilder(std::string_view name) SE_REQUIRES(g_main);
  SceneBuilder& Add(const SpriteRenderer& sprite, Vec2 pos, float rot = 0.0F,
                    Vec2 scale = {1.0F, 1.0F}) SE_REQUIRES(g_main);
  [[nodiscard]] std::unique_ptr<World> Finish() SE_REQUIRES(g_main);

 private:
  std::unique_ptr<World> world_;
  std::uint32_t next_ = 1;
};

// A sprite of the whole |w| x |h| texture |name| with a centred origin.
[[nodiscard]] SpriteRenderer SpriteOf(std::string_view name, float w, float h,
                                      Rgba8 tint = {});

// A canvas of |w| x |h| world pixels at zoom 1.
[[nodiscard]] Camera2D CanvasCamera(float w, float h);

// Loads "player" (assets/player/player.png, 48 x 48) and makes "checker"
// (16 x 16: opaque red and half-transparent blue squares of 4 px) and
// "white" (1 x 1), then uploads them. False when one could not be loaded
// or uploaded.
[[nodiscard]] bool LoadTestTextures(GpuResources& resources)
    SE_REQUIRES(g_main);

// The repository root (SE_SOURCE_DIR).
[[nodiscard]] std::string RepoRoot();

// Draws |layers| offscreen into a composite of |width| x |height| (clear
// opaque black) and reads it back.
[[nodiscard]] TexturePayload RenderOffscreen(
    Renderer2D& renderer, std::span<const RenderLayer> layers,
    std::uint32_t width, std::uint32_t height) SE_REQUIRES(g_main);

// The pixel at (x, y).
[[nodiscard]] Rgba8 PixelAt(const TexturePayload& image, std::uint32_t x,
                            std::uint32_t y);

}  // namespace se::testing

#endif  // TESTS_RENDER_GPU_SUPPORT_GPU_FIXTURE_H_
