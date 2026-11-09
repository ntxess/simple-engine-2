// Copyright 2026 The simple-engine-2 Authors.

#include "se_render/frame_recorder.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_render/gpu_device.h"

namespace se {

std::expected<FrameRecorder, Error> FrameRecorder::Prepare(GpuDevice& device) {
  SE_ASSERT_MAIN_THREAD();
  SE_ASSERT_MSG(device.ClaimedWindow() != nullptr,
                "FrameRecorder needs a claimed window");
  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device.Native());
  if (commands == nullptr) {
    std::string message = "SDL_AcquireGPUCommandBuffer: ";
    message += SDL_GetError();
    return Fail(ErrorCode::kUnsupported, std::move(message));
  }
  ++device.MutableStats().frames;
  return FrameRecorder(&device, commands);
}

FrameRecorder::FrameRecorder(FrameRecorder&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      commands_(std::exchange(other.commands_, nullptr)),
      swapchain_(std::exchange(other.swapchain_, nullptr)),
      width_(other.width_),
      height_(other.height_),
      acquired_(other.acquired_) {}

SDL_GPUTexture* FrameRecorder::AcquireSwapchain() noexcept {
  SE_ASSERT_MAIN_THREAD();
  SE_ASSERT_MSG(!acquired_, "AcquireSwapchain called twice in one frame");
  acquired_ = true;
  GpuFrameStats& stats = device_->MutableStats();
  if (device_->TakeForcedNullAcquire()) {
    ++stats.forced_null_acquires;
    ++stats.null_acquires;
    return nullptr;
  }
  SDL_Window* window = device_->ClaimedWindow();
  // A failed wait still lets the acquire report NULL or an error.
  SDL_WaitForGPUSwapchain(device_->Native(), window);
  if (!SDL_AcquireGPUSwapchainTexture(commands_, window, &swapchain_, &width_,
                                      &height_)) {
    ++stats.acquire_failures;
    swapchain_ = nullptr;
  }
  if (swapchain_ == nullptr) {
    ++stats.null_acquires;
  }
  return swapchain_;
}

void FrameRecorder::Clear(const ClearColor& color) noexcept {
  if (swapchain_ == nullptr) return;
  SDL_GPUColorTargetInfo target{};
  target.texture = swapchain_;
  target.clear_color = SDL_FColor{color.r, color.g, color.b, color.a};
  target.load_op = SDL_GPU_LOADOP_CLEAR;
  target.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* pass =
      SDL_BeginGPURenderPass(commands_, &target, 1, nullptr);
  if (pass != nullptr) SDL_EndGPURenderPass(pass);
}

FrameRecorder::~FrameRecorder() {
  if (commands_ == nullptr) return;
  SE_ASSERT_MAIN_THREAD();
  // Always submit: cancelling after an acquire is forbidden, and a buffer
  // without an acquired texture submits as an empty frame.
  if (!SDL_SubmitGPUCommandBuffer(commands_)) {
    ++device_->MutableStats().submit_failures;
  } else if (swapchain_ != nullptr) {
    ++device_->MutableStats().presented;
  }
  commands_ = nullptr;
}

}  // namespace se
