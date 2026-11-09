// Copyright 2026 The simple-engine-2 Authors.
//
// RenderPresentPort: the PresentPort over M6a's se::present
// (se_render/present.h) and M7's frame transaction (Renderer2D,
// FrameRecorder), written at the M6/M7 integration
// (docs/milestone-specs.md, "Integration (M6a + M6b + M7)"; ADR-066,
// ADR-067). It replaces M6b's ClearOnlyPresentPort.
//
//   QueryCaps ... SetPresentMode   se::present, field by field. Without
//                       the SDL patch every query is unsupported, except
//                       that IMMEDIATE (tearing) and the present mode come
//                       from SDL itself, so VRR runs unpatched (ADR-058).
//   OnFrameSubmitted    nothing to keep: Renderer2D keeps each submitted
//                       frame's fence (FrameRecorder::TakeFence) in its
//                       ring; a fence handed in anyway is released.
//   WaitFrameFence      the fence of the submission |frames_back| ago
//                       (Renderer2D::FrameFence); SDL has no timed fence
//                       wait, so a timeout above 0 blocks in
//                       SDL_WaitForGPUFences.
//   RepresentLast       Renderer2D::RepresentLast, the blit of the last
//                       composite (HalfRate in Standard).
//   WaitSwapchain       SDL_WaitForGPUSwapchain.
//
// The GPU timing calls are the present API's; the frame transaction makes
// them itself (FrameRecorder's INTEGRATION(M6) hooks) and Renderer2D reads
// the results, so the pacer never needs them.
//
// Test hook: InjectDeviceLost() makes the next QueryDeviceStatus report
// kRemoved once (--inject-device-lost).
//
// Main thread only. The device and the renderer outlive the port; after a
// device loss the App makes a new port for the new device.

#ifndef SE_RUNTIME_RENDER_PRESENT_PORT_H_
#define SE_RUNTIME_RENDER_PRESENT_PORT_H_

#include <cstdint>
#include <optional>

#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_render/gpu_device.h"
#include "se_render/renderer_2d.h"
#include "se_runtime/present_port.h"

namespace se {

class RenderPresentPort final : public PresentPort {
 public:
  RenderPresentPort(GpuDevice& device, Renderer2D& renderer)
      SE_REQUIRES(g_main);

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

  // The next QueryDeviceStatus reports kRemoved (the test hook).
  void InjectDeviceLost() noexcept { inject_removed_ = true; }
  [[nodiscard]] SwapchainMode PresentMode() const noexcept { return mode_; }

 private:
  GpuDevice* device_ = nullptr;
  Renderer2D* renderer_ = nullptr;
  SwapchainMode mode_ = SwapchainMode::kVsync;
  bool inject_removed_ = false;
};

}  // namespace se

#endif  // SE_RUNTIME_RENDER_PRESENT_PORT_H_
