// Copyright 2026 The simple-engine-2 Authors.
//
// PresentPort: what the FramePacer needs from presentation
// (docs/milestone-specs.md M6, "The present API"). The operations mirror
// se::present (se_render/present.h, M6a), plus WaitFrameFence for the N-L
// fence wait, RepresentLast for HalfRate's Standard re-present,
// WaitSwapchain for Standard's SDL_WaitForGPUSwapchain and OnFrameSubmitted,
// which hands the port each frame's fence. The pacer never includes
// present.h: until the integration writes the adapter over se::present and
// M7's FrameRecorder, the runtime uses ClearOnlyPresentPort
// (clear_present_port.h), and the unit tests a fake (tests/support).
//
// Every operation runs on the main thread. A port reports what it cannot
// do through QueryCaps() and its return values ("unsupported" is never an
// error): an unpatched SDL has no waitable, no present statistics, no GPU
// timestamps, no native handles and no device-removed query.
//
// This header has no SDL include, so the pure PacerPolicy can use its data
// types; the SDL handles are forward declared.

#ifndef SE_RUNTIME_PRESENT_PORT_H_
#define SE_RUNTIME_PRESENT_PORT_H_

#include <cstdint>
#include <optional>
#include <string_view>

struct SDL_GPUCommandBuffer;
struct SDL_GPUFence;

namespace se {

// se::present::Caps.
struct PresentCaps {
  // Built with the SDL presentation patch.
  bool patched = false;
  // D3D12's frame-latency waitable; Vulkan's present_wait.
  bool frame_latency_waitable = false;
  // Displayed-refresh statistics (DXGI_FRAME_STATISTICS).
  bool present_stats = false;
  // Per-command-buffer begin/end timestamps.
  bool gpu_timestamps = false;
  bool native_handles = false;
  // The device-removed query.
  bool device_status = false;
  // IMMEDIATE with ALLOW_TEARING (VRR).
  bool tearing = false;
};

// se::present::WaitResult.
enum class PresentWait : std::uint8_t { kReady, kTimeout, kUnsupported };

// se::present::PresentStats: DXGI_FRAME_STATISTICS semantics.
struct PresentStatistics {
  // The id of the last present that was displayed (presents count from 1).
  std::uint64_t present_count = 0;
  // The refresh that showed that present.
  std::uint64_t present_refresh_count = 0;
  // A refresh and its vblank time (SDL_GetTicksNS domain).
  std::uint64_t sync_refresh_count = 0;
  std::uint64_t sync_time_ns = 0;
  // The id of the last present issued (GetLastPresentCount).
  std::uint64_t last_present_count = 0;
};

// se::present::GpuTime.
struct GpuTiming {
  std::uint64_t duration_ns = 0;
};

// se::present::DeviceStatus.
enum class GpuDeviceStatus : std::uint8_t { kOk, kRemoved, kUnknown };

// se::present::NativeHandles: borrowed, valid while the device lives.
struct PresentNativeHandles {
  void* d3d12_device = nullptr;
  void* d3d12_queue = nullptr;
  void* dxgi_swapchain = nullptr;
  void* vk_instance = nullptr;
  void* vk_physical_device = nullptr;
  void* vk_device = nullptr;
  void* vk_queue = nullptr;
  std::uint32_t vk_queue_family = 0;
  void* vk_swapchain = nullptr;
};

// The swapchain present modes the pacer uses (SDL_GPUPresentMode).
enum class SwapchainMode : std::uint8_t { kVsync, kImmediate, kMailbox };

[[nodiscard]] constexpr std::string_view SwapchainModeName(
    SwapchainMode mode) noexcept {
  switch (mode) {
    case SwapchainMode::kVsync:
      return "vsync";
    case SwapchainMode::kImmediate:
      return "immediate";
    case SwapchainMode::kMailbox:
      return "mailbox";
  }
  return "?";
}

class PresentPort {
 public:
  PresentPort() = default;
  PresentPort(const PresentPort&) = delete;
  PresentPort& operator=(const PresentPort&) = delete;
  virtual ~PresentPort() = default;

  [[nodiscard]] virtual PresentCaps QueryCaps() = 0;

  // 1 or 2 frames between CPU and display; creates the waitable on first
  // use. False when unsupported.
  virtual bool SetFrameLatency(int frames) = 0;
  // Waits on the frame-latency waitable, never longer than |timeout_ns|.
  virtual PresentWait WaitFrameLatency(std::uint64_t timeout_ns) = 0;

  // False before the first displayed present, while minimized, or when
  // unsupported.
  virtual bool GetPresentStats(PresentStatistics& stats) = 0;

  // GPU duration of one command buffer: Begin after it is acquired, End
  // right before submit, Read after its fence signalled (nullopt before,
  // or when unsupported).
  virtual void BeginGpuTiming(SDL_GPUCommandBuffer* commands) noexcept = 0;
  virtual void EndGpuTiming(SDL_GPUCommandBuffer* commands) noexcept = 0;
  [[nodiscard]] virtual std::optional<GpuTiming> ReadGpuTime(
      SDL_GPUFence* fence) = 0;

  [[nodiscard]] virtual GpuDeviceStatus QueryDeviceStatus() = 0;
  virtual bool GetNativeHandles(PresentNativeHandles& handles) = 0;

  // VSYNC (Paced, Standard) or IMMEDIATE (VRR) for the claimed window.
  virtual bool SetPresentMode(SwapchainMode mode) = 0;

  // M6b's additions.
  //
  // A frame was submitted; |fence| (owned by the port from now on) signals
  // when its GPU work is done. nullptr when the recorder gives no fence
  // (M5's clear-only FrameRecorder).
  virtual void OnFrameSubmitted(SDL_GPUFence* fence) noexcept = 0;
  // Waits for the fence of the frame |frames_back| submits ago (1: the
  // last one), never longer than |timeout_ns| where the backend allows a
  // bounded wait. kReady when that frame's GPU work is done (or no such
  // frame exists), kTimeout when |timeout_ns| passed first, kUnsupported
  // when the port keeps no fences.
  virtual PresentWait WaitFrameFence(int frames_back,
                                     std::uint64_t timeout_ns) = 0;
  // HalfRate in Standard: presents the last composite again, without a new
  // RenderList. False when nothing could be presented.
  virtual bool RepresentLast() = 0;
  // Standard's wait for a swapchain image (SDL_WaitForGPUSwapchain).
  virtual bool WaitSwapchain() = 0;
};

}  // namespace se

#endif  // SE_RUNTIME_PRESENT_PORT_H_
