// Copyright 2026 The simple-engine-2 Authors.
//
// se-fault-boundary: RunContained catches what the build phase throws (the
// standard library's exceptions: bad_alloc while growing buffers, the
// injected build and vertices faults), so that nothing unwinds into the
// recording phase or SDL's frames (ADR-006 lists the FrameRecorder
// transaction as a boundary). Nothing else here catches.

#include "se_render/frame_recorder.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_timer.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <string>
#include <utility>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_render/frame_faults.h"
#include "se_render/gpu_device.h"
#include "se_render/gpu_resources.h"
#include "src/gpu_assert.h"

namespace se {
namespace {

constexpr std::size_t kFaultTextSize = 256;

void CopyText(char (&out)[kFaultTextSize], const char* text) noexcept {
  std::size_t n = 0;
  if (text != nullptr) {
    while (n + 1 < kFaultTextSize && text[n] != '\0') {
      out[n] = text[n];
      ++n;
    }
  }
  out[n] = '\0';
}

}  // namespace

std::expected<void, Error> FrameRecorder::RunContained(
    void (*run)(void* context), void* context) {
  // The handlers copy the text into a fixed buffer (ADR-038): a fault from
  // exhausted memory must not need memory to be reported.
  char what[kFaultTextSize] = {};
  bool faulted = false;
  try {
    run(context);
  } catch (const std::exception& error) {
    CopyText(what, error.what());
    faulted = true;
  } catch (...) {
    CopyText(what, "an exception that is not a std::exception");
    faulted = true;
  }
  if (!faulted) return {};
  return Fail(ErrorCode::kInvalidArgument,
              std::string("render build fault: ") + what);
}

std::expected<FrameRecorder, Error> FrameRecorder::Prepare(
    GpuDevice& device, GpuResources& resources, FrameFaults& faults,
    const PrepareOptions& options) {
  SE_ASSERT_GPU_THREAD();
  FrameRecorder recorder(&device, &resources, &faults);
  if (options.frame) {
    const bool injected = faults.Fires(FrameStep::kPrepare);
    SDL_GPUCommandBuffer* commands =
        injected ? nullptr : SDL_AcquireGPUCommandBuffer(device.Native());
    if (commands == nullptr) {
      recorder.done_ = true;
      return Fail(ErrorCode::kUnsupported,
                  injected ? std::string("SDL_AcquireGPUCommandBuffer: the "
                                         "injected prepare fault")
                           : std::string("SDL_AcquireGPUCommandBuffer: ") +
                                 SDL_GetError());
    }
    recorder.commands_ = commands;
    ++device.MutableStats().frames;
    // INTEGRATION(M6): present::BeginGpuTiming(device, commands), so the
    // frame's GPU duration feeds g_hi in FrameCosts.
  }
  if (options.uploads) {
    const bool injected = faults.Fires(FrameStep::kUpload);
    recorder.upload_commands_ =
        injected ? nullptr : SDL_AcquireGPUCommandBuffer(device.Native());
    recorder.record_.upload_commands = recorder.upload_commands_ != nullptr;
  }
  return recorder;
}

FrameRecorder::FrameRecorder(FrameRecorder&& other) noexcept
    : device_(other.device_),
      resources_(other.resources_),
      faults_(other.faults_),
      commands_(std::exchange(other.commands_, nullptr)),
      upload_commands_(std::exchange(other.upload_commands_, nullptr)),
      copy_pass_(std::exchange(other.copy_pass_, nullptr)),
      render_pass_(std::exchange(other.render_pass_, nullptr)),
      swapchain_(std::exchange(other.swapchain_, nullptr)),
      fence_(std::exchange(other.fence_, nullptr)),
      width_(other.width_),
      height_(other.height_),
      acquire_attempted_(other.acquire_attempted_),
      done_(std::exchange(other.done_, true)),
      record_(other.record_) {}

FrameRecorder::~FrameRecorder() {
  if (done_ && fence_ == nullptr) return;
  SE_ASSERT_GPU_THREAD();
  Submit();
  if (fence_ != nullptr) {
    resources_->ReleaseFence(std::exchange(fence_, nullptr));
  }
}

void FrameRecorder::CancelUnacquired(SDL_GPUCommandBuffer*& commands,
                                     bool swapchain_acquired) noexcept {
  SE_ASSERT_MSG(!swapchain_acquired,
                "SDL_CancelGPUCommandBuffer after a swapchain acquire");
  if (commands == nullptr) return;
  SDL_CancelGPUCommandBuffer(commands);
  commands = nullptr;
}

bool FrameRecorder::SubmitUploads() noexcept {
  SE_ASSERT_GPU_THREAD();
  if (upload_commands_ == nullptr) {
    resources_->DropUploadPlan();
    return false;
  }
  if (!resources_->RecordUploads(upload_commands_, false)) {
    // Nothing was recorded and no swapchain texture is ever acquired on
    // the upload command buffer, so cancelling it is allowed.
    CancelUnacquired(upload_commands_, false);
    resources_->DropUploadPlan();
    return false;
  }
  SDL_GPUFence* fence =
      SDL_SubmitGPUCommandBufferAndAcquireFence(upload_commands_);
  upload_commands_ = nullptr;
  resources_->UploadsSubmitted(fence);
  record_.uploads_submitted = fence != nullptr;
  return fence != nullptr;
}

SDL_GPUCopyPass* FrameRecorder::BeginCopyPass() noexcept {
  SE_ASSERT_GPU_THREAD();
  SE_ASSERT_MSG(copy_pass_ == nullptr && render_pass_ == nullptr,
                "a pass is already open");
  if (commands_ == nullptr || faults_->Fires(FrameStep::kCopyPass)) {
    return nullptr;
  }
  copy_pass_ = SDL_BeginGPUCopyPass(commands_);
  return copy_pass_;
}

void FrameRecorder::EndCopyPass() noexcept {
  if (copy_pass_ == nullptr) return;
  SE_ASSERT_GPU_THREAD();
  SDL_EndGPUCopyPass(std::exchange(copy_pass_, nullptr));
}

SDL_GPURenderPass* FrameRecorder::BeginRenderPass(
    const SDL_GPUColorTargetInfo& target, bool swapchain) noexcept {
  SE_ASSERT_GPU_THREAD();
  SE_ASSERT_MSG(copy_pass_ == nullptr && render_pass_ == nullptr,
                "a pass is already open");
  const FrameStep step =
      swapchain ? FrameStep::kSwapchainPass : FrameStep::kViewportPass;
  if (commands_ == nullptr || target.texture == nullptr ||
      faults_->Fires(step)) {
    return nullptr;
  }
  render_pass_ = SDL_BeginGPURenderPass(commands_, &target, 1, nullptr);
  return render_pass_;
}

void FrameRecorder::EndRenderPass() noexcept {
  if (render_pass_ == nullptr) return;
  SE_ASSERT_GPU_THREAD();
  SDL_EndGPURenderPass(std::exchange(render_pass_, nullptr));
}

SDL_GPUTexture* FrameRecorder::AcquireSwapchain() noexcept {
  SE_ASSERT_GPU_THREAD();
  SE_ASSERT_MSG(!acquire_attempted_,
                "AcquireSwapchain called twice in one frame");
  acquire_attempted_ = true;
  SDL_Window* window = device_->ClaimedWindow();
  if (commands_ == nullptr || window == nullptr) return nullptr;
  GpuFrameStats& stats = device_->MutableStats();
  if (faults_->Fires(FrameStep::kAcquire)) {
    ++stats.null_acquires;
    return nullptr;
  }
  if (device_->TakeForcedNullAcquire()) {
    ++stats.forced_null_acquires;
    ++stats.null_acquires;
    return nullptr;
  }
  const std::uint64_t start = SDL_GetTicksNS();
  // A failed wait still lets the acquire report NULL or an error.
  SDL_WaitForGPUSwapchain(device_->Native(), window);
  if (SDL_AcquireGPUSwapchainTexture(commands_, window, &swapchain_, &width_,
                                     &height_)) {
    record_.swapchain_acquired = true;
  } else {
    ++stats.acquire_failures;
    swapchain_ = nullptr;
  }
  record_.swapchain_wait_ns = SDL_GetTicksNS() - start;
  if (swapchain_ == nullptr) ++stats.null_acquires;
  return swapchain_;
}

bool FrameRecorder::Submit() noexcept {
  if (done_) return record_.submitted;
  SE_ASSERT_GPU_THREAD();
  done_ = true;
  EndCopyPass();
  EndRenderPass();
  if (upload_commands_ != nullptr) SubmitUploads();
  bool ok = true;
  if (commands_ != nullptr) {
    // INTEGRATION(M6): present::EndGpuTiming(*device_, commands_) here, and
    // present::ReadGpuTime(*device_, fence) once this frame's fence has
    // signalled (Renderer2D's fence ring, at a later frame start).
    const std::uint64_t start = SDL_GetTicksNS();
    // Always submit: cancelling after an acquire is forbidden, and a
    // buffer without an acquired texture submits as an offscreen frame.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands_);
    commands_ = nullptr;
    record_.submit_ns = SDL_GetTicksNS() - start;
    resources_->CountFence(fence);
    GpuFrameStats& stats = device_->MutableStats();
    if (fence == nullptr || faults_->Fires(FrameStep::kSubmit)) {
      // The injected fault submits for real (the buffer may hold an
      // acquired swapchain texture) and reports the failure SDL would.
      ok = false;
      record_.submit_failed = true;
      ++stats.submit_failures;
      if (fence != nullptr) resources_->ReleaseFence(fence);
    } else {
      fence_ = fence;
      record_.submitted = true;
      if (swapchain_ != nullptr) ++stats.presented;
    }
  }
  resources_->RunRetireQueue();
  return ok;
}

}  // namespace se
