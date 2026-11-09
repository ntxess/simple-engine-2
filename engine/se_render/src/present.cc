// Copyright 2026 The simple-engine-2 Authors.

#include "se_render/present.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>

#include <bit>
#include <cstdint>
#include <optional>

#include "se_core/assert.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_render/gpu_device.h"

#if !defined(SE_SDL_PATCHED)
#error "se_render is built with SE_SDL_PATCHED=0 or 1 (cmake: SE_SDL_PATCH)"
#endif

#if SE_SDL_PATCHED
#include <SDL3/SDL_gpu_se.h>

static_assert(SDL_SE_GPU_PRESENTATION_VERSION == 1,
              "present.cc is written against version 1 of SDL_gpu_se.h");
#endif

namespace se::present {

#if SE_SDL_PATCHED

namespace {

// A frame latency SDL_SE_SetGPUFrameLatency accepts and the pacer uses.
constexpr int kMinFrameLatency = 1;
constexpr int kMaxFrameLatency = 2;

}  // namespace

Caps QueryCaps(GpuDevice& device) {
  SE_ASSERT_MAIN_THREAD();
  Caps caps;
  caps.patched = true;
  const SDL_SE_GPUCapabilityFlags flags =
      SDL_SE_GetGPUCapabilities(device.Native(), device.ClaimedWindow());
  caps.frame_latency_waitable =
      (flags & SDL_SE_GPU_CAPABILITY_FRAME_LATENCY_WAIT) != 0;
  caps.present_stats = (flags & SDL_SE_GPU_CAPABILITY_PRESENT_STATISTICS) != 0;
  caps.gpu_timestamps = (flags & SDL_SE_GPU_CAPABILITY_TIMESTAMPS) != 0;
  caps.native_handles = (flags & SDL_SE_GPU_CAPABILITY_NATIVE_HANDLES) != 0;
  caps.device_status = (flags & SDL_SE_GPU_CAPABILITY_DEVICE_STATUS) != 0;
  caps.tearing = (flags & SDL_SE_GPU_CAPABILITY_TEARING) != 0;
  return caps;
}

bool SetFrameLatency(GpuDevice& device, int frames) {
  SE_ASSERT_MAIN_THREAD();
  SE_ASSERT_MSG(frames >= kMinFrameLatency && frames <= kMaxFrameLatency,
                "the frame latency is 1 or 2");
  if (device.ClaimedWindow() == nullptr) return false;
  if (!SDL_SE_SetGPUFrameLatency(device.Native(), device.ClaimedWindow(),
                                 static_cast<Uint32>(frames))) {
    SE_LOG_WARN("SetFrameLatency({}) on {}: {}", frames, device.Driver(),
                SDL_GetError());
    return false;
  }
  return true;
}

WaitResult WaitFrameLatency(GpuDevice& device, std::uint64_t timeout_ns) {
  SE_ASSERT_MAIN_THREAD();
  if (device.ClaimedWindow() == nullptr) return WaitResult::kUnsupported;
  switch (SDL_SE_WaitForGPUFrameLatency(device.Native(), device.ClaimedWindow(),
                                        timeout_ns)) {
    case SDL_SE_GPU_WAIT_READY:
      return WaitResult::kReady;
    case SDL_SE_GPU_WAIT_TIMEOUT:
      return WaitResult::kTimeout;
    case SDL_SE_GPU_WAIT_UNSUPPORTED:
      break;
  }
  return WaitResult::kUnsupported;
}

bool GetPresentStats(GpuDevice& device, PresentStats& stats) {
  SE_ASSERT_MAIN_THREAD();
  if (device.ClaimedWindow() == nullptr) return false;
  SDL_SE_GPUPresentStatistics statistics{};
  if (!SDL_SE_GetGPUPresentStatistics(device.Native(), device.ClaimedWindow(),
                                      &statistics)) {
    return false;
  }
  stats.present_count = statistics.present_count;
  stats.present_refresh_count = statistics.present_refresh_count;
  stats.sync_refresh_count = statistics.sync_refresh_count;
  stats.sync_time_ns = statistics.sync_time_ns;
  stats.last_present_count = statistics.last_present_count;
  return true;
}

void BeginGpuTiming(GpuDevice& /*device*/,
                    SDL_GPUCommandBuffer* commands) noexcept {
  SE_ASSERT_MAIN_THREAD();
  if (commands != nullptr) SDL_SE_BeginGPUTiming(commands);
}

void EndGpuTiming(GpuDevice& /*device*/,
                  SDL_GPUCommandBuffer* commands) noexcept {
  SE_ASSERT_MAIN_THREAD();
  if (commands != nullptr) SDL_SE_EndGPUTiming(commands);
}

std::optional<GpuTime> ReadGpuTime(GpuDevice& device, SDL_GPUFence* fence) {
  SE_ASSERT_MAIN_THREAD();
  if (fence == nullptr) return std::nullopt;
  Uint64 duration_ns = 0;
  if (!SDL_SE_GetGPUTiming(device.Native(), fence, &duration_ns)) {
    return std::nullopt;
  }
  return GpuTime{duration_ns};
}

DeviceStatus QueryDeviceStatus(GpuDevice& device) {
  SE_ASSERT_MAIN_THREAD();
  switch (SDL_SE_GetGPUDeviceStatus(device.Native())) {
    case SDL_SE_GPU_DEVICE_STATUS_OK:
      return DeviceStatus::kOk;
    case SDL_SE_GPU_DEVICE_STATUS_REMOVED:
      return DeviceStatus::kRemoved;
    case SDL_SE_GPU_DEVICE_STATUS_UNKNOWN:
      break;
  }
  return DeviceStatus::kUnknown;
}

bool GetNativeHandles(GpuDevice& device, NativeHandles& handles) {
  SE_ASSERT_MAIN_THREAD();
  SDL_SE_GPUNativeHandles native{};
  if (!SDL_SE_GetGPUNativeHandles(device.Native(), device.ClaimedWindow(),
                                  &native)) {
    return false;
  }
  handles.d3d12_device = native.d3d12_device;
  handles.d3d12_queue = native.d3d12_queue;
  handles.dxgi_swapchain = native.dxgi_swapchain;
  handles.vk_instance = native.vk_instance;
  handles.vk_physical_device = native.vk_physical_device;
  handles.vk_device = native.vk_device;
  handles.vk_queue = native.vk_queue;
  handles.vk_queue_family = native.vk_queue_family;
  // A non-dispatchable Vulkan handle: 64 bits on every platform the engine
  // builds for, the width of a pointer there.
  static_assert(sizeof(void*) == sizeof(native.vk_swapchain));
  handles.vk_swapchain = std::bit_cast<void*>(native.vk_swapchain);
  return true;
}

bool SetPresentMode(GpuDevice& device, SDL_GPUPresentMode mode) {
  SE_ASSERT_MAIN_THREAD();
  SDL_Window* window = device.ClaimedWindow();
  if (window == nullptr) return false;
  if (!SDL_WindowSupportsGPUPresentMode(device.Native(), window, mode)) {
    return false;
  }
  if (!SDL_SetGPUSwapchainParameters(device.Native(), window,
                                     SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)) {
    SE_LOG_WARN("SetPresentMode({}) on {}: {}", static_cast<int>(mode),
                device.Driver(), SDL_GetError());
    return false;
  }
  return true;
}

#else  // !SE_SDL_PATCHED: everything unsupported, every action a no-op.

Caps QueryCaps(GpuDevice& /*device*/) {
  SE_ASSERT_MAIN_THREAD();
  return Caps{};
}

bool SetFrameLatency(GpuDevice& /*device*/, int /*frames*/) {
  SE_ASSERT_MAIN_THREAD();
  return false;
}

WaitResult WaitFrameLatency(GpuDevice& /*device*/,
                            std::uint64_t /*timeout_ns*/) {
  SE_ASSERT_MAIN_THREAD();
  return WaitResult::kUnsupported;
}

bool GetPresentStats(GpuDevice& /*device*/, PresentStats& /*stats*/) {
  SE_ASSERT_MAIN_THREAD();
  return false;
}

void BeginGpuTiming(GpuDevice& /*device*/,
                    SDL_GPUCommandBuffer* /*commands*/) noexcept {
  SE_ASSERT_MAIN_THREAD();
}

void EndGpuTiming(GpuDevice& /*device*/,
                  SDL_GPUCommandBuffer* /*commands*/) noexcept {
  SE_ASSERT_MAIN_THREAD();
}

std::optional<GpuTime> ReadGpuTime(GpuDevice& /*device*/,
                                   SDL_GPUFence* /*fence*/) {
  SE_ASSERT_MAIN_THREAD();
  return std::nullopt;
}

DeviceStatus QueryDeviceStatus(GpuDevice& /*device*/) {
  SE_ASSERT_MAIN_THREAD();
  return DeviceStatus::kUnknown;
}

bool GetNativeHandles(GpuDevice& /*device*/, NativeHandles& /*handles*/) {
  SE_ASSERT_MAIN_THREAD();
  return false;
}

bool SetPresentMode(GpuDevice& /*device*/, SDL_GPUPresentMode /*mode*/) {
  SE_ASSERT_MAIN_THREAD();
  return false;
}

#endif  // SE_SDL_PATCHED

}  // namespace se::present
