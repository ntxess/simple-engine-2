// Copyright 2026 The simple-engine-2 Authors.
//
// The present API (docs/milestone-specs.md M6, "The present API"): the
// presentation signals of the SDL presentation patch
// (cmake/patches/sdl3-3.4.20-se-presentation.patch, SDL_gpu_se.h) for the
// window a GpuDevice has claimed. It is the contract between M6a (this
// wrapper and the patch) and M6b (the FramePacer, which reaches it only
// through its PresentPort; the integration step writes that adapter).
//
// Every function asserts the main thread: SDL_GPU performs no thread
// checks, and the swapchain belongs to the window's thread. In an unpatched
// build (SE_SDL_PATCH=OFF, compile definition SE_SDL_PATCHED=0; preset
// msvc-unpatched-dev) every query reports "unsupported" and every action is
// a no-op that returns false; the engine then runs Standard pacing.
//
// Backends (ADR-050 to ADR-053):
// - Direct3D 12: the DXGI frame-latency waitable (the swapchain is recreated
//   with FRAME_LATENCY_WAITABLE_OBJECT on the first SetFrameLatency, then
//   SetMaximumFrameLatency), GetFrameStatistics and GetLastPresentCount,
//   timestamp queries, GetDeviceRemovedReason, ALLOW_TEARING with sync
//   interval 0 for IMMEDIATE.
// - Vulkan: VK_KHR_present_id and VK_KHR_present_wait (the waitable),
//   VK_GOOGLE_display_timing (statistics), vkCmdWriteTimestamp, and
//   VK_ERROR_DEVICE_LOST. A missing extension reports its capability as
//   unsupported; it never fails device creation.

#ifndef SE_RENDER_PRESENT_H_
#define SE_RENDER_PRESENT_H_

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <optional>

#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_render/gpu_device.h"

namespace se::present {

struct Caps {
  bool patched = false;                 // built with the patch
  bool frame_latency_waitable = false;  // D3D12 waitable, Vulkan present_wait
  bool present_stats = false;           // displayed-refresh statistics
  bool gpu_timestamps = false;          // per-command-buffer begin/end
  bool native_handles = false;
  bool device_status = false;  // device-removed query
  bool tearing = false;        // IMMEDIATE with ALLOW_TEARING (VRR)
};
// The window-dependent capabilities (present statistics, tearing) are for
// the claimed window; without one they are reported for the device alone.
[[nodiscard]] Caps QueryCaps(GpuDevice& device) SE_REQUIRES(g_main);

// 1 or 2; creates the waitable on first use (D3D12: the swapchain is
// recreated with FRAME_LATENCY_WAITABLE_OBJECT, then
// SetMaximumFrameLatency). False when unsupported.
bool SetFrameLatency(GpuDevice& device, int frames) SE_REQUIRES(g_main);
enum class WaitResult : std::uint8_t { kReady, kTimeout, kUnsupported };
// D3D12: WaitForSingleObjectEx on the waitable. Vulkan: vkWaitForPresentKHR
// for the present |frames| back. Never longer than |timeout_ns|.
[[nodiscard]] WaitResult WaitFrameLatency(GpuDevice& device,
                                          std::uint64_t timeout_ns)
    SE_REQUIRES(g_main);

struct PresentStats {                       // DXGI_FRAME_STATISTICS semantics
  std::uint64_t present_count = 0;          // presents the swapchain issued
  std::uint64_t present_refresh_count = 0;  // refresh that showed the last
                                            // displayed present
  std::uint64_t sync_refresh_count = 0;
  std::uint64_t sync_time_ns = 0;        // that vblank, SDL_GetTicksNS
                                         // domain
  std::uint64_t last_present_count = 0;  // GetLastPresentCount
};
// False before the first displayed present, while minimized, or when
// unsupported (Vulkan without VK_GOOGLE_display_timing or present_id).
// present_count is the present count of the most recently displayed present
// (DXGI's PresentCount), in the numbering of last_present_count.
[[nodiscard]] bool GetPresentStats(GpuDevice& device, PresentStats& stats)
    SE_REQUIRES(g_main);

// GPU duration of one command buffer. Begin after the command buffer is
// acquired, End right before submit; Read after its fence signalled. Begin
// and End are outside any pass; the timestamps are resolved at submit, and
// a missing End is written there. The fence is the one
// SDL_SubmitGPUCommandBufferAndAcquireFence returned; a successful Read
// consumes the result.
void BeginGpuTiming(GpuDevice& device, SDL_GPUCommandBuffer* commands) noexcept
    SE_REQUIRES(g_main);
void EndGpuTiming(GpuDevice& device, SDL_GPUCommandBuffer* commands) noexcept
    SE_REQUIRES(g_main);
struct GpuTime {
  std::uint64_t duration_ns = 0;
};
[[nodiscard]] std::optional<GpuTime> ReadGpuTime(GpuDevice& device,
                                                 SDL_GPUFence* fence)
    SE_REQUIRES(g_main);

enum class DeviceStatus : std::uint8_t { kOk, kRemoved, kUnknown };
// kUnknown when unsupported.
[[nodiscard]] DeviceStatus QueryDeviceStatus(GpuDevice& device)
    SE_REQUIRES(g_main);

struct NativeHandles {  // borrowed; valid while the device lives
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
// The swapchain handles are those of the claimed window (null without one)
// and change when SDL recreates the swapchain (resize, present mode).
bool GetNativeHandles(GpuDevice& device, NativeHandles& handles)
    SE_REQUIRES(g_main);

// IMMEDIATE (tearing) or VSYNC for the claimed window; VRR uses IMMEDIATE.
bool SetPresentMode(GpuDevice& device, SDL_GPUPresentMode mode)
    SE_REQUIRES(g_main);

}  // namespace se::present

#endif  // SE_RENDER_PRESENT_H_
