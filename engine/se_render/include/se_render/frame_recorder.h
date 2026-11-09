// Copyright 2026 The simple-engine-2 Authors.
//
// The frame transaction, clear-only in M5 (docs/design/mt-design.md
// "Rendering", change 10; M7 adds the RenderList passes):
//
//   auto frame = FrameRecorder::Prepare(device);   // may fail: Error
//   if (frame) {
//     if (frame->AcquireSwapchain() != nullptr) frame->Clear(colour);
//   }                                              // ~FrameRecorder submits
//
// Everything that can fail or throw runs before the acquire, in Prepare
// (the command buffer). The wait and the acquire are noexcept; so is
// recording (one render pass that clears the swapchain texture). The
// destructor always submits and never cancels (SDL forbids
// SDL_CancelGPUCommandBuffer after an acquire). A NULL swapchain texture
// skips the present; it is not an error and never a state change (a
// visible window can return NULL when too many frames are in flight).
// GpuDevice::ForceNullAcquires is the test hook that makes acquires return
// NULL for N frames.

#ifndef SE_RENDER_FRAME_RECORDER_H_
#define SE_RENDER_FRAME_RECORDER_H_

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <expected>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_render/gpu_device.h"

namespace se {

struct ClearColor {
  float r = 0.0F;
  float g = 0.0F;
  float b = 0.0F;
  float a = 1.0F;
};

class FrameRecorder {
 public:
  // Acquires the frame's command buffer for the device's claimed window.
  [[nodiscard]] static std::expected<FrameRecorder, Error> Prepare(
      GpuDevice& device) SE_REQUIRES(g_main);

  FrameRecorder(FrameRecorder&& other) noexcept;
  FrameRecorder& operator=(FrameRecorder&&) = delete;
  FrameRecorder(const FrameRecorder&) = delete;
  FrameRecorder& operator=(const FrameRecorder&) = delete;
  // Submits the command buffer (presenting when a texture was acquired).
  ~FrameRecorder();

  // Waits for a swapchain image (SDL_WaitForGPUSwapchain) and acquires it.
  // NULL skips the present. Call at most once.
  SDL_GPUTexture* AcquireSwapchain() noexcept;
  // One render pass that clears the acquired texture to |color|; nothing
  // without a texture.
  void Clear(const ClearColor& color) noexcept;

  [[nodiscard]] SDL_GPUTexture* Swapchain() const noexcept {
    return swapchain_;
  }
  [[nodiscard]] std::uint32_t Width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t Height() const noexcept { return height_; }

 private:
  FrameRecorder(GpuDevice* device, SDL_GPUCommandBuffer* commands) noexcept
      : device_(device), commands_(commands) {}

  GpuDevice* device_ = nullptr;
  SDL_GPUCommandBuffer* commands_ = nullptr;
  SDL_GPUTexture* swapchain_ = nullptr;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  bool acquired_ = false;
};

}  // namespace se

#endif  // SE_RENDER_FRAME_RECORDER_H_
