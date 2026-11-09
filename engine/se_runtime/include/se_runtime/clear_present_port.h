// Copyright 2026 The simple-engine-2 Authors.
//
// ClearOnlyPresentPort: the PresentPort over M5's GpuDevice and its
// clear-only FrameRecorder, until the integration writes the adapter over
// se::present (M6a) and M7's FrameRecorder (docs/milestone-specs.md M6,
// "The present API"). It reports what the unpatched SDL really offers:
// IMMEDIATE when SDL_WindowSupportsGPUPresentMode says so (DXGI tearing on
// D3D12), and nothing else: no waitable, no present statistics, no GPU
// timestamps, no native handles, no device-removed query. So the pacer
// runs Standard, or VRR when asked.
//
// Fences. M5's FrameRecorder submits without one, so after each frame the
// port submits an empty command buffer with SDL_SubmitGPUCommandBufferAnd
// AcquireFence: its fence signals once the frame's work before it on the
// queue is done, which is what the N-L wait needs. SDL has no timed fence
// wait, so WaitFrameFence polls with a zero timeout and otherwise blocks
// in SDL_WaitForGPUFences (a GPU wait, which main may do).
//
// RepresentLast is Renderer2D::RepresentLast, the blit of the last
// composite (M7, merged before the integration's adapter).
//
// Main thread only; destroy it before the device (it releases its fences).

#ifndef SE_RUNTIME_CLEAR_PRESENT_PORT_H_
#define SE_RUNTIME_CLEAR_PRESENT_PORT_H_

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_render/gpu_device.h"
#include "se_render/renderer_2d.h"
#include "se_runtime/present_port.h"

namespace se {

class ClearOnlyPresentPort final : public PresentPort {
 public:
  // Fences kept: enough for frame latency 2 and one more.
  static constexpr std::size_t kFences = 3;

  // |renderer| outlives the port.
  ClearOnlyPresentPort(GpuDevice& device, Renderer2D& renderer)
      SE_REQUIRES(g_main);
  ~ClearOnlyPresentPort() override;

  PresentCaps QueryCaps() override;
  bool SetFrameLatency(int frames) override;
  PresentWait WaitFrameLatency(std::uint64_t timeout_ns) override;
  bool GetPresentStats(PresentStatistics& stats) override;
  void BeginGpuTiming(SDL_GPUCommandBuffer* commands) noexcept override;
  void EndGpuTiming(SDL_GPUCommandBuffer* commands) noexcept override;
  std::optional<GpuTiming> ReadGpuTime(SDL_GPUFence* fence) override;
  GpuDeviceStatus QueryDeviceStatus() override;
  bool GetNativeHandles(PresentNativeHandles& handles) override;
  bool SetPresentMode(SwapchainMode mode) override;
  void OnFrameSubmitted(SDL_GPUFence* fence) noexcept override;
  PresentWait WaitFrameFence(int frames_back,
                             std::uint64_t timeout_ns) override;
  bool RepresentLast() override;
  bool WaitSwapchain() override;

  [[nodiscard]] SwapchainMode PresentMode() const noexcept { return mode_; }
  // Frames presented by RepresentLast.
  [[nodiscard]] std::uint64_t Represents() const noexcept {
    return represents_;
  }

 private:
  void Keep(SDL_GPUFence* fence) noexcept;

  GpuDevice* device_ = nullptr;
  Renderer2D* renderer_ = nullptr;
  SwapchainMode mode_ = SwapchainMode::kVsync;
  // The newest fence is fences_[(head_ + size_ - 1) % kFences].
  std::array<SDL_GPUFence*, kFences> fences_{};
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  std::uint64_t represents_ = 0;
  std::uint64_t fence_failures_ = 0;
};

}  // namespace se

#endif  // SE_RUNTIME_CLEAR_PRESENT_PORT_H_
