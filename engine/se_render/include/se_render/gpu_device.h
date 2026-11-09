// Copyright 2026 The simple-engine-2 Authors.
//
// The SDL_GPU device (docs/milestone-specs.md M5, se_render). Created with
// the DXIL and SPIR-V shader formats; the driver is "direct3d12" on Windows
// and "vulkan" on Linux unless --gpu-driver or SE_GPU_DRIVER names another
// (ResolveGpuDriver). Debug mode is on in Debug and Dev builds; when the
// driver's debug layer is missing, the device is created without it.
//
// SDL_GPU performs no thread checks, and the swapchain belongs to the
// window's thread, so every call asserts the main thread. The device claims
// the window with SDL_GPU_PRESENTMODE_VSYNC (M5 presents "Standard" without
// the frame-latency waitable; M6 adds the patch and the pacer).

#ifndef SE_RENDER_GPU_DEVICE_H_
#define SE_RENDER_GPU_DEVICE_H_

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se {

// The platform's default driver: "direct3d12" on Windows, "vulkan" on
// Linux.
[[nodiscard]] std::string_view DefaultGpuDriver() noexcept;

// The driver to create: |command_line| (--gpu-driver) when not empty, else
// |environment| (SE_GPU_DRIVER) when not empty, else DefaultGpuDriver().
[[nodiscard]] std::string ResolveGpuDriver(std::string_view command_line,
                                           std::string_view environment);

struct GpuDeviceConfig {
  // Empty: DefaultGpuDriver().
  std::string driver;
  // On in Debug and Dev builds (asserts on), off in Release.
  bool debug_mode = SE_ASSERTS != 0;
};

// Counters of the frame transaction (FrameRecorder), for logs and tests.
struct GpuFrameStats {
  std::uint64_t frames = 0;
  std::uint64_t presented = 0;
  // NULL swapchain textures (minimized window, too many frames in flight,
  // or forced by the test hook). Never a state change.
  std::uint64_t null_acquires = 0;
  std::uint64_t forced_null_acquires = 0;
  std::uint64_t acquire_failures = 0;
  std::uint64_t submit_failures = 0;
};

class GpuDevice {
 public:
  [[nodiscard]] static std::expected<GpuDevice, Error> Create(
      const GpuDeviceConfig& config) SE_REQUIRES(g_main);

  GpuDevice(GpuDevice&& other) noexcept;
  GpuDevice& operator=(GpuDevice&& other) noexcept;
  GpuDevice(const GpuDevice&) = delete;
  GpuDevice& operator=(const GpuDevice&) = delete;
  // Releases the claimed window, then destroys the device.
  ~GpuDevice();

  // Claims |window| for presentation with SDL_GPU_PRESENTMODE_VSYNC. One
  // window per device in M5.
  [[nodiscard]] std::expected<void, Error> ClaimWindow(SDL_Window* window)
      SE_REQUIRES(g_main);
  // Releases the claimed window (shutdown step 12, before the window is
  // destroyed).
  void ReleaseWindow() noexcept;
  // SDL_WaitForGPUIdle (shutdown step 4).
  void WaitForIdle() noexcept;

  [[nodiscard]] SDL_GPUDevice* Native() const noexcept { return device_; }
  [[nodiscard]] SDL_Window* ClaimedWindow() const noexcept { return window_; }
  [[nodiscard]] std::string_view Driver() const noexcept { return driver_; }
  [[nodiscard]] bool DebugMode() const noexcept { return debug_mode_; }

  // Test hook: the next |frames| swapchain acquires return NULL without
  // calling SDL, as a minimized window or a busy swapchain would.
  void ForceNullAcquires(std::uint32_t frames) noexcept {
    forced_null_acquires_ = frames;
  }
  // Consumes one forced NULL acquire; true if there was one.
  bool TakeForcedNullAcquire() noexcept;

  [[nodiscard]] const GpuFrameStats& Stats() const noexcept { return stats_; }
  [[nodiscard]] GpuFrameStats& MutableStats() noexcept { return stats_; }

 private:
  GpuDevice(SDL_GPUDevice* device, std::string driver,
            bool debug_mode) noexcept;
  void Release() noexcept;

  SDL_GPUDevice* device_ = nullptr;
  SDL_Window* window_ = nullptr;
  std::string driver_;
  bool debug_mode_ = false;
  std::uint32_t forced_null_acquires_ = 0;
  GpuFrameStats stats_;
};

}  // namespace se

#endif  // SE_RENDER_GPU_DEVICE_H_
