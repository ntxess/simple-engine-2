// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/clear_present_port.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstdint>
#include <optional>

#include "se_core/assert.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_render/frame_recorder.h"
#include "se_render/gpu_device.h"
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

ClearOnlyPresentPort::ClearOnlyPresentPort(GpuDevice& device,
                                           const ClearColor& color)
    : device_(&device), color_(color) {
  AssertMainThread();
  SE_ASSERT_MSG(device.ClaimedWindow() != nullptr,
                "the present port needs a claimed window");
}

ClearOnlyPresentPort::~ClearOnlyPresentPort() {
  if (size_ == 0) return;
  AssertMainThread();
  for (std::size_t i = 0; i < size_; ++i) {
    SDL_ReleaseGPUFence(device_->Native(), fences_[(head_ + i) % kFences]);
  }
  size_ = 0;
}

PresentCaps ClearOnlyPresentPort::QueryCaps() {
  AssertMainThread();
  PresentCaps caps;
  caps.tearing = SDL_WindowSupportsGPUPresentMode(
      device_->Native(), device_->ClaimedWindow(),
      SDL_GPU_PRESENTMODE_IMMEDIATE);
  return caps;
}

bool ClearOnlyPresentPort::SetFrameLatency(int /*frames*/) { return false; }

PresentWait ClearOnlyPresentPort::WaitFrameLatency(
    std::uint64_t /*timeout_ns*/) {
  return PresentWait::kUnsupported;
}

bool ClearOnlyPresentPort::GetPresentStats(PresentStatistics& /*stats*/) {
  return false;
}

void ClearOnlyPresentPort::BeginGpuTiming(
    SDL_GPUCommandBuffer* /*commands*/) noexcept {}

void ClearOnlyPresentPort::EndGpuTiming(
    SDL_GPUCommandBuffer* /*commands*/) noexcept {}

std::optional<GpuTiming> ClearOnlyPresentPort::ReadGpuTime(
    SDL_GPUFence* /*fence*/) {
  return std::nullopt;
}

GpuDeviceStatus ClearOnlyPresentPort::QueryDeviceStatus() {
  return GpuDeviceStatus::kUnknown;
}

bool ClearOnlyPresentPort::GetNativeHandles(PresentNativeHandles& /*handles*/) {
  return false;
}

bool ClearOnlyPresentPort::SetPresentMode(SwapchainMode mode) {
  AssertMainThread();
  SDL_GPUDevice* device = device_->Native();
  SDL_Window* window = device_->ClaimedWindow();
  const SDL_GPUPresentMode sdl_mode = ToSdl(mode);
  if (!SDL_WindowSupportsGPUPresentMode(device, window, sdl_mode)) {
    return false;
  }
  if (!SDL_SetGPUSwapchainParameters(
          device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, sdl_mode)) {
    SE_LOG_WARN("SDL_SetGPUSwapchainParameters({}): {}",
                SwapchainModeName(mode), SDL_GetError());
    return false;
  }
  mode_ = mode;
  return true;
}

void ClearOnlyPresentPort::Keep(SDL_GPUFence* fence) noexcept {
  if (size_ == kFences) {
    SDL_ReleaseGPUFence(device_->Native(), fences_[head_]);
    head_ = (head_ + 1) % kFences;
    --size_;
  }
  fences_[(head_ + size_) % kFences] = fence;
  ++size_;
}

void ClearOnlyPresentPort::OnFrameSubmitted(SDL_GPUFence* fence) noexcept {
  AssertMainThread();
  if (fence != nullptr) {
    Keep(fence);
    return;
  }
  // An empty command buffer behind the frame: its fence signals when the
  // frame's work on the queue is done.
  SDL_GPUCommandBuffer* commands =
      SDL_AcquireGPUCommandBuffer(device_->Native());
  if (commands == nullptr) {
    ++fence_failures_;
    return;
  }
  SDL_GPUFence* trailing = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
  if (trailing == nullptr) {
    ++fence_failures_;
    return;
  }
  Keep(trailing);
}

PresentWait ClearOnlyPresentPort::WaitFrameFence(int frames_back,
                                                 std::uint64_t timeout_ns) {
  AssertMainThread();
  const auto back = static_cast<std::size_t>(frames_back < 1 ? 1 : frames_back);
  if (back > size_) return PresentWait::kReady;
  SDL_GPUFence* fence = fences_[(head_ + size_ - back) % kFences];
  if (SDL_QueryGPUFence(device_->Native(), fence)) return PresentWait::kReady;
  if (timeout_ns == 0) return PresentWait::kTimeout;
  // SDL has no timed wait; the GPU work behind a clear is short.
  SDL_WaitForGPUFences(device_->Native(), /*wait_all=*/true, &fence, 1);
  return PresentWait::kReady;
}

bool ClearOnlyPresentPort::RepresentLast() {
  AssertMainThread();
  bool presented = false;
  {
    auto frame = FrameRecorder::Prepare(*device_);
    if (!frame) return false;
    if (frame->AcquireSwapchain() != nullptr) {
      frame->Clear(color_);
      presented = true;
    }
    // ~FrameRecorder submits here.
  }
  if (presented) ++represents_;
  return presented;
}

bool ClearOnlyPresentPort::WaitSwapchain() {
  AssertMainThread();
  return SDL_WaitForGPUSwapchain(device_->Native(), device_->ClaimedWindow());
}

}  // namespace se
