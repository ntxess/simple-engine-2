// Copyright 2026 The simple-engine-2 Authors.

#include "se_render/gpu_device.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"

namespace se {
namespace {

Error GpuError(std::string_view what) {
  std::string message(what);
  message += ": ";
  message += SDL_GetError();
  return Error{ErrorCode::kUnsupported, std::move(message), {}, 0, 0};
}

}  // namespace

std::string_view DefaultGpuDriver() noexcept {
#if defined(_WIN32)
  return "direct3d12";
#else
  return "vulkan";
#endif
}

std::string ResolveGpuDriver(std::string_view command_line,
                             std::string_view environment) {
  if (!command_line.empty()) return std::string(command_line);
  if (!environment.empty()) return std::string(environment);
  return std::string(DefaultGpuDriver());
}

std::expected<GpuDevice, Error> GpuDevice::Create(
    const GpuDeviceConfig& config) {
  SE_ASSERT_MAIN_THREAD();
  const std::string driver =
      config.driver.empty() ? std::string(DefaultGpuDriver()) : config.driver;
  constexpr SDL_GPUShaderFormat kFormats =
      SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV;
  bool debug_mode = config.debug_mode;
  SDL_GPUDevice* device =
      SDL_CreateGPUDevice(kFormats, debug_mode, driver.c_str());
  if (device == nullptr && debug_mode) {
    // The driver's debug layer is optional (Graphics Tools, validation
    // layers); run without it rather than not at all.
    SE_LOG_WARN(
        "GPU device ({}) with debug mode failed: {}; retrying "
        "without debug mode",
        driver, SDL_GetError());
    debug_mode = false;
    device = SDL_CreateGPUDevice(kFormats, debug_mode, driver.c_str());
  }
  if (device == nullptr) {
    return std::unexpected(GpuError("SDL_CreateGPUDevice(" + driver + ")"));
  }
  const char* actual = SDL_GetGPUDeviceDriver(device);
  return GpuDevice(device, actual != nullptr ? std::string(actual) : driver,
                   debug_mode);
}

GpuDevice::GpuDevice(SDL_GPUDevice* device, std::string driver,
                     bool debug_mode) noexcept
    : device_(device), driver_(std::move(driver)), debug_mode_(debug_mode) {}

GpuDevice::GpuDevice(GpuDevice&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      window_(std::exchange(other.window_, nullptr)),
      driver_(std::move(other.driver_)),
      debug_mode_(other.debug_mode_),
      forced_null_acquires_(other.forced_null_acquires_),
      stats_(other.stats_) {}

GpuDevice& GpuDevice::operator=(GpuDevice&& other) noexcept {
  if (this != &other) {
    Release();
    device_ = std::exchange(other.device_, nullptr);
    window_ = std::exchange(other.window_, nullptr);
    driver_ = std::move(other.driver_);
    debug_mode_ = other.debug_mode_;
    forced_null_acquires_ = other.forced_null_acquires_;
    stats_ = other.stats_;
  }
  return *this;
}

GpuDevice::~GpuDevice() { Release(); }

void GpuDevice::Release() noexcept {
  if (device_ == nullptr) return;
  SE_ASSERT_MAIN_THREAD();
  ReleaseWindow();
  SDL_DestroyGPUDevice(device_);
  device_ = nullptr;
}

std::expected<void, Error> GpuDevice::ClaimWindow(SDL_Window* window) {
  SE_ASSERT_MAIN_THREAD();
  SE_ASSERT_MSG(window_ == nullptr, "the GPU device already claims a window");
  if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
    return std::unexpected(GpuError("SDL_ClaimWindowForGPUDevice"));
  }
  window_ = window;
  if (!SDL_SetGPUSwapchainParameters(device_, window,
                                     SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                     SDL_GPU_PRESENTMODE_VSYNC)) {
    Error error = GpuError("SDL_SetGPUSwapchainParameters(VSYNC)");
    ReleaseWindow();
    return std::unexpected(std::move(error));
  }
  return {};
}

void GpuDevice::ReleaseWindow() noexcept {
  if (window_ == nullptr || device_ == nullptr) return;
  SE_ASSERT_MAIN_THREAD();
  SDL_ReleaseWindowFromGPUDevice(device_, window_);
  window_ = nullptr;
}

void GpuDevice::WaitForIdle() noexcept {
  if (device_ == nullptr) return;
  SE_ASSERT_MAIN_THREAD();
  SDL_WaitForGPUIdle(device_);
}

bool GpuDevice::TakeForcedNullAcquire() noexcept {
  if (forced_null_acquires_ == 0) return false;
  --forced_null_acquires_;
  return true;
}

}  // namespace se
