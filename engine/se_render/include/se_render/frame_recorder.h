// Copyright 2026 The simple-engine-2 Authors.
//
// The frame transaction (docs/milestone-specs.md M7, frame_recorder.h;
// docs/design/mt-design.md "Rendering", "The frame transaction"; red team
// C2, C3, C10). It replaces M5's clear-only recorder; Renderer2D drives it:
//
//   1. Build phase, which may throw: RenderLists, vertices, batches,
//      buffer growth (RunBuildPhase). Nothing GPU-side is acquired yet. A
//      throw is contained here, the frame's only fault boundary, and comes
//      back as an Error; the game build then quits through the callbacks'
//      fault path, the editor (M10) enters Play.Faulted.
//   2. Prepare: the frame command buffer (failure is an Error: the frame is
//      skipped and logged), then the upload command buffer when uploads are
//      pending (failure only delays the uploads).
//   3. The uploads (submitted at once with their own fence), the vertex
//      copy pass, the offscreen viewport passes and the composite, then the
//      swapchain acquire. A NULL swapchain texture skips the swapchain pass
//      and is never a state change; the command buffer is still submitted.
//   4. The swapchain pass (the blit).
//   5. The guard (the destructor, or Submit()) ends any open pass, submits
//      with SDL_SubmitGPUCommandBufferAndAcquireFence, never cancels after
//      an acquire (asserted), keeps the fence for the pacer's N-L wait
//      (TakeFence) and runs the retire queue.
//
// Steps 2 to 5 are noexcept: buffers are pre-sized in step 1 and only
// engine code runs. The swapchain is acquired after SDL_WaitForGPUSwapchain,
// which returns at once after the FramePacer's own waits (M6). The frame
// command buffer is timed with se::present (BeginGpuTiming after it is
// acquired, EndGpuTiming right before its submit, outside any pass);
// Renderer2D reads the duration once the fence has signalled (M6/M7
// integration). Every step consults FrameFaults (the --inject-frame-fault
// test hook).

#ifndef SE_RENDER_FRAME_RECORDER_H_
#define SE_RENDER_FRAME_RECORDER_H_

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_render/frame_faults.h"
#include "se_render/gpu_device.h"
#include "se_render/gpu_resources.h"

namespace se {

struct ClearColor {
  float r = 0.0F;
  float g = 0.0F;
  float b = 0.0F;
  float a = 1.0F;
};

struct PrepareOptions {
  // Acquire the frame command buffer (false: uploads only).
  bool frame = true;
  // Acquire the upload command buffer (uploads are pending).
  bool uploads = false;
};

// What one transaction did, for Renderer2D's stats.
struct FrameRecord {
  bool upload_commands = false;
  bool uploads_submitted = false;
  bool swapchain_acquired = false;
  bool submitted = false;
  bool submit_failed = false;
  std::uint64_t swapchain_wait_ns = 0;
  std::uint64_t submit_ns = 0;
};

class FrameRecorder {
 public:
  // Step 1: runs |build| (anything callable as void()) inside the fault
  // boundary. A throw from it is returned as an Error (kInvalidArgument,
  // "render build fault: <what>").
  template <class F>
  [[nodiscard]] static std::expected<void, Error> RunBuildPhase(F& build) {
    return RunContained([](void* context) { (*static_cast<F*>(context))(); },
                        static_cast<void*>(&build));
  }

  // Step 2.
  [[nodiscard]] static std::expected<FrameRecorder, Error> Prepare(
      GpuDevice& device, GpuResources& resources, FrameFaults& faults,
      const PrepareOptions& options) SE_REQUIRES(g_main);

  FrameRecorder(FrameRecorder&& other) noexcept;
  FrameRecorder& operator=(FrameRecorder&&) = delete;
  FrameRecorder(const FrameRecorder&) = delete;
  FrameRecorder& operator=(const FrameRecorder&) = delete;
  // Step 5 if Submit() has not run.
  ~FrameRecorder();

  // The frame command buffer; null for an upload-only transaction.
  [[nodiscard]] SDL_GPUCommandBuffer* Commands() const noexcept {
    return commands_;
  }
  [[nodiscard]] bool HasUploadCommands() const noexcept {
    return upload_commands_ != nullptr;
  }

  // Step 3: records the planned uploads into the upload command buffer
  // and submits it with its own fence (GpuResources::UploadsSubmitted).
  // Without an upload command buffer the plan is dropped. Returns whether
  // copies were submitted.
  bool SubmitUploads() noexcept SE_REQUIRES(g_main);

  // A copy pass on the frame command buffer; NULL on failure (and for the
  // copy_pass fault). One pass is open at a time.
  [[nodiscard]] SDL_GPUCopyPass* BeginCopyPass() noexcept SE_REQUIRES(g_main);
  void EndCopyPass() noexcept SE_REQUIRES(g_main);
  // A render pass on one colour target; NULL on failure (the
  // viewport_pass fault for offscreen targets, swapchain_pass for the
  // swapchain).
  [[nodiscard]] SDL_GPURenderPass* BeginRenderPass(
      const SDL_GPUColorTargetInfo& target, bool swapchain) noexcept
      SE_REQUIRES(g_main);
  void EndRenderPass() noexcept SE_REQUIRES(g_main);

  // Waits for a swapchain image (SDL_WaitForGPUSwapchain) and acquires it
  // for the device's claimed window. NULL skips the present: a minimized
  // window, too many frames in flight, the acquire fault or
  // GpuDevice::ForceNullAcquires. Call at most once; NULL without a
  // claimed window.
  SDL_GPUTexture* AcquireSwapchain() noexcept SE_REQUIRES(g_main);
  [[nodiscard]] SDL_GPUTexture* Swapchain() const noexcept {
    return swapchain_;
  }
  [[nodiscard]] std::uint32_t SwapchainWidth() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t SwapchainHeight() const noexcept {
    return height_;
  }

  // Step 5 now: ends any open pass, submits the upload and frame command
  // buffers (never cancels after an acquire), keeps the frame's fence and
  // runs the retire queue. Returns false when the submit failed. Later
  // calls do nothing.
  bool Submit() noexcept SE_REQUIRES(g_main);
  // The submitted frame's fence, owned by the caller from now on (already
  // counted by GpuResources; release it with GpuResources::ReleaseFence).
  // Null before Submit, after a failed one, or for an upload-only
  // transaction.
  [[nodiscard]] SDL_GPUFence* TakeFence() noexcept {
    return std::exchange(fence_, nullptr);
  }
  [[nodiscard]] const FrameRecord& Record() const noexcept { return record_; }

 private:
  FrameRecorder(GpuDevice* device, GpuResources* resources,
                FrameFaults* faults) noexcept
      : device_(device), resources_(resources), faults_(faults) {}

  // The fault boundary (frame_recorder.cc). Not noexcept: should building
  // the Error itself run out of memory, that throw goes on to the SDL
  // callbacks' boundary.
  [[nodiscard]] static std::expected<void, Error> RunContained(
      void (*run)(void* context), void* context);
  // Cancels a command buffer on which no swapchain texture was acquired;
  // asserts that none was (SDL forbids cancelling after an acquire).
  void CancelUnacquired(SDL_GPUCommandBuffer*& commands,
                        bool swapchain_acquired) noexcept SE_REQUIRES(g_main);

  GpuDevice* device_ = nullptr;
  GpuResources* resources_ = nullptr;
  FrameFaults* faults_ = nullptr;
  SDL_GPUCommandBuffer* commands_ = nullptr;
  SDL_GPUCommandBuffer* upload_commands_ = nullptr;
  SDL_GPUCopyPass* copy_pass_ = nullptr;
  SDL_GPURenderPass* render_pass_ = nullptr;
  SDL_GPUTexture* swapchain_ = nullptr;
  SDL_GPUFence* fence_ = nullptr;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  bool acquire_attempted_ = false;
  bool done_ = false;
  FrameRecord record_;
};

}  // namespace se

#endif  // SE_RENDER_FRAME_RECORDER_H_
