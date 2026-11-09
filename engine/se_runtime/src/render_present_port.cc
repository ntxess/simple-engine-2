// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/render_present_port.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstdint>
#include <optional>

#include "se_core/assert.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_render/gpu_device.h"
#include "se_render/present.h"
#include "se_render/renderer_2d.h"
#include "se_runtime/present_port.h"

namespace se {
namespace {

SDL_GPUPresentMode ToSdl(SwapchainMode mode) noexcept {
  switch (mode) {
    case SwapchainMode::kVsync:
      return SDL_GPU_PRESENTMODE_VSYNC;
    case SwapchainMode::kImmediate:
      return SDL_GPU_PRESENTMODE_IMMEDIATE;
    case SwapchainMode::kMailbox:
      return SDL_GPU_PRESENTMODE_MAILBOX;
  }
  return SDL_GPU_PRESENTMODE_VSYNC;
}

}  // namespace

RenderPresentPort::RenderPresentPort(GpuDevice& device, Renderer2D& renderer)
    : device_(&device), renderer_(&renderer) {
  AssertMainThread();
  SE_ASSERT_MSG(device.ClaimedWindow() != nullptr,
                "the present port needs a claimed window");
}

PresentCaps RenderPresentPort::QueryCaps() {
  AssertMainThread();
  const present::Caps from = present::QueryCaps(*device_);
  PresentCaps caps;
  caps.patched = from.patched;
  caps.frame_latency_waitable = from.frame_latency_waitable;
  caps.present_stats = from.present_stats;
  caps.gpu_timestamps = from.gpu_timestamps;
  caps.native_handles = from.native_handles;
  caps.device_status = from.device_status;
  caps.tearing = from.tearing;
  if (!from.patched) {
    // SDL itself presents IMMEDIATE with tearing where DXGI allows it.
    caps.tearing = SDL_WindowSupportsGPUPresentMode(
        device_->Native(), device_->ClaimedWindow(),
        SDL_GPU_PRESENTMODE_IMMEDIATE);
  }
  return caps;
}

bool RenderPresentPort::SetFrameLatency(int frames) {
  AssertMainThread();
  return present::SetFrameLatency(*device_, frames);
}

PresentWait RenderPresentPort::WaitFrameLatency(std::uint64_t timeout_ns) {
  AssertMainThread();
  switch (present::WaitFrameLatency(*device_, timeout_ns)) {
    case present::WaitResult::kReady:
      return PresentWait::kReady;
    case present::WaitResult::kTimeout:
      return PresentWait::kTimeout;
    case present::WaitResult::kUnsupported:
      break;
  }
  return PresentWait::kUnsupported;
}

bool RenderPresentPort::GetPresentStats(PresentStatistics& stats) {
  AssertMainThread();
  present::PresentStats from;
  if (!present::GetPresentStats(*device_, from)) return false;
  stats.present_count = from.present_count;
  stats.present_refresh_count = from.present_refresh_count;
  stats.sync_refresh_count = from.sync_refresh_count;
  stats.sync_time_ns = from.sync_time_ns;
  stats.last_present_count = from.last_present_count;
  return true;
}

void RenderPresentPort::BeginGpuTiming(
    SDL_GPUCommandBuffer* commands) noexcept {
  AssertMainThread();
  present::BeginGpuTiming(*device_, commands);
}

void RenderPresentPort::EndGpuTiming(SDL_GPUCommandBuffer* commands) noexcept {
  AssertMainThread();
  present::EndGpuTiming(*device_, commands);
}

std::optional<GpuTiming> RenderPresentPort::ReadGpuTime(SDL_GPUFence* fence) {
  AssertMainThread();
  const std::optional<present::GpuTime> time =
      present::ReadGpuTime(*device_, fence);
  if (!time) return std::nullopt;
  return GpuTiming{time->duration_ns};
}

GpuDeviceStatus RenderPresentPort::QueryDeviceStatus() {
  AssertMainThread();
  if (inject_removed_) {
    inject_removed_ = false;
    return GpuDeviceStatus::kRemoved;
  }
  switch (present::QueryDeviceStatus(*device_)) {
    case present::DeviceStatus::kOk:
      return GpuDeviceStatus::kOk;
    case present::DeviceStatus::kRemoved:
      return GpuDeviceStatus::kRemoved;
    case present::DeviceStatus::kUnknown:
      break;
  }
  return GpuDeviceStatus::kUnknown;
}

bool RenderPresentPort::GetNativeHandles(PresentNativeHandles& handles) {
  AssertMainThread();
  present::NativeHandles from;
  if (!present::GetNativeHandles(*device_, from)) return false;
  handles.d3d12_device = from.d3d12_device;
  handles.d3d12_queue = from.d3d12_queue;
  handles.dxgi_swapchain = from.dxgi_swapchain;
  handles.vk_instance = from.vk_instance;
  handles.vk_physical_device = from.vk_physical_device;
  handles.vk_device = from.vk_device;
  handles.vk_queue = from.vk_queue;
  handles.vk_queue_family = from.vk_queue_family;
  handles.vk_swapchain = from.vk_swapchain;
  return true;
}

bool RenderPresentPort::SetPresentMode(SwapchainMode mode) {
  AssertMainThread();
  const SDL_GPUPresentMode sdl_mode = ToSdl(mode);
  bool set = false;
  if (present::QueryCaps(*device_).patched) {
    set = present::SetPresentMode(*device_, sdl_mode);
  } else {
    // Without the patch the present API's actions are no-ops; SDL's own
    // call sets the mode (ADR-058: VRR runs unpatched).
    SDL_GPUDevice* device = device_->Native();
    SDL_Window* window = device_->ClaimedWindow();
    set = SDL_WindowSupportsGPUPresentMode(device, window, sdl_mode) &&
          SDL_SetGPUSwapchainParameters(
              device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, sdl_mode);
    if (!set) {
      SE_LOG_WARN("SDL_SetGPUSwapchainParameters({}): {}",
                  SwapchainModeName(mode), SDL_GetError());
    }
  }
  if (set) mode_ = mode;
  return set;
}

void RenderPresentPort::OnFrameSubmitted(SDL_GPUFence* fence) noexcept {
  AssertMainThread();
  // Renderer2D keeps the frame's fence itself.
  if (fence != nullptr) renderer_->Resources().ReleaseFence(fence);
}

PresentWait RenderPresentPort::WaitFrameFence(int frames_back,
                                              std::uint64_t timeout_ns) {
  AssertMainThread();
  const auto back = static_cast<std::size_t>(frames_back < 1 ? 1 : frames_back);
  SDL_GPUFence* fence = renderer_->FrameFence(back);
  if (fence == nullptr) return PresentWait::kReady;
  SDL_GPUDevice* device = device_->Native();
  if (SDL_QueryGPUFence(device, fence)) return PresentWait::kReady;
  if (timeout_ns == 0) return PresentWait::kTimeout;
  // SDL has no timed fence wait.
  SDL_WaitForGPUFences(device, /*wait_all=*/true, &fence, 1);
  return PresentWait::kReady;
}

bool RenderPresentPort::RepresentLast() {
  AssertMainThread();
  return renderer_->RepresentLast().presented;
}

bool RenderPresentPort::WaitSwapchain() {
  AssertMainThread();
  return SDL_WaitForGPUSwapchain(device_->Native(), device_->ClaimedWindow());
}

}  // namespace se
