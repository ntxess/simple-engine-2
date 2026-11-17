// Copyright 2026 The simple-engine-2 Authors.
//
// Fakes for the FramePacer's unit tests (docs/milestone-specs.md M6b): a
// virtual clock whose sleeps advance time, and a PresentPort presenting to
// a virtual fixed-refresh display with DXGI_FRAME_STATISTICS semantics. No
// SDL, no GPU, no thread.
//
// The display: vblank k at anchor + k * period. A present issued at time t
// shows at the first vblank after t, and never before one refresh after
// the previous present (the flip queue); LatePresents(n) makes the next n
// presents show one refresh late, as DXGI flip model can (red team P1).
// GetPresentStats names the newest present shown by now and the newest
// vblank as the sync pair (with caps.present_stats; a composed window has
// none). The waitable and the swapchain wait return when the newest
// present has been shown (frame latency 1), wake_latency_ns after its
// vblank; a zero-timeout waitable call polls. With model_frame_latency the
// waits follow the latency the pacer set (M10a): at frame latency L they
// return once the L-th newest present has been shown, so L - 1 presents
// may wait in the queue.

#ifndef TESTS_SUPPORT_FAKE_PRESENT_PORT_H_
#define TESTS_SUPPORT_FAKE_PRESENT_PORT_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "se_platform/vblank_sampler.h"
#include "se_runtime/frame_pacer.h"
#include "se_runtime/present_port.h"

namespace se::testing {

class FakePacerClock final : public PacerClock {
 public:
  std::uint64_t NowNs() noexcept override { return now_ns; }
  void SleepNs(std::uint64_t ns) noexcept override;
  void Advance(std::uint64_t ns) noexcept { now_ns += ns; }

  std::uint64_t now_ns = 10'000'000'000ULL;
  // Every sleep wakes this much late.
  std::uint64_t late_ns = 0;
  // Sleeps so far, and the last one's length.
  std::uint64_t sleep_count = 0;
  std::uint64_t last_sleep_ns = 0;
};

class FakePresentPort final : public PresentPort {
 public:
  // The refresh count of vblank 0.
  static constexpr std::uint64_t kRefreshBase = 1000;

  FakePresentPort(FakePacerClock* clock, double period_ns);

  // The test's frame transaction presents through this.
  void Present();
  // The next |count| presents show one refresh late.
  void LatePresents(std::uint32_t count) { late_presents_ = count; }
  // The vblank at or before |now_ns|.
  [[nodiscard]] std::uint64_t LatestVblank(std::uint64_t now_ns) const;
  // A sampler reading that agrees with the display (period * |scale|); its
  // stamps trail the vblanks by sampler_lag_ns.
  [[nodiscard]] VblankReading Reading(std::uint64_t now_ns,
                                      double scale = 1.0) const;
  // The refresh present |id| (1-based) showed at, or nullopt.
  [[nodiscard]] std::optional<std::uint64_t> ShownRefresh(
      std::uint64_t id) const;
  // The refresh of the vblank at or before |time_ns| (a vblank time
  // falls on its own refresh).
  [[nodiscard]] std::uint64_t RefreshAt(std::uint64_t time_ns) const;
  [[nodiscard]] std::uint64_t VblankAt(std::uint64_t refresh) const;

  PresentCaps QueryCaps() override { return caps; }
  bool SetFrameLatency(int frames) override;
  PresentWait WaitFrameLatency(std::uint64_t timeout_ns) override;
  bool GetPresentStats(PresentStatistics& stats) override;
  void BeginGpuTiming(
      [[maybe_unused]] SDL_GPUCommandBuffer* commands) noexcept override {}
  void EndGpuTiming(
      [[maybe_unused]] SDL_GPUCommandBuffer* commands) noexcept override {}
  std::optional<GpuTiming> ReadGpuTime(
      [[maybe_unused]] SDL_GPUFence* fence) override {
    return std::nullopt;
  }
  GpuDeviceStatus QueryDeviceStatus() override {
    return GpuDeviceStatus::kUnknown;
  }
  bool GetNativeHandles(
      [[maybe_unused]] PresentNativeHandles& handles) override {
    return false;
  }
  bool SetPresentMode(SwapchainMode mode) override;
  void OnFrameSubmitted(SDL_GPUFence* fence) noexcept override;
  PresentWait WaitFrameFence(int frames_back,
                             std::uint64_t timeout_ns) override;
  bool RepresentLast() override;
  bool WaitSwapchain() override;

  // What QueryCaps returns.
  PresentCaps caps;
  bool present_mode_result = true;
  // The next fence poll reports pending (cleared by a waiting call).
  bool fence_pending = false;
  // The waits return by the frame latency last set (SetFrameLatency)
  // instead of always by latency 1.
  bool model_frame_latency = false;
  // How long a wake after a flip takes.
  std::uint64_t wake_latency_ns = 100'000;
  // How long after a vblank the sampler stamps it.
  std::uint64_t sampler_lag_ns = 0;

  // Calls the pacer made.
  std::vector<int> latencies;
  std::vector<SwapchainMode> present_modes;
  std::uint32_t waitable_waits = 0;
  std::uint32_t waitable_polls = 0;
  std::uint32_t swapchain_waits = 0;
  std::uint32_t represents = 0;
  std::uint32_t fence_polls = 0;
  std::uint32_t fence_waits = 0;
  std::uint32_t submitted = 0;
  // Waits and polls (latency, fence, swapchain) the pacer made without the
  // in-driver flag up (M12: it raises it around each).
  std::uint32_t waits_outside_driver = 0;

 private:
  struct Shown {
    std::uint64_t id = 0;
    std::uint64_t refresh = 0;
  };
  void WaitForNewestShown();
  [[nodiscard]] bool NewestShown() const;
  // The present the waits wait for: the newest (frame latency 1), or the
  // L-th newest with model_frame_latency; null when fewer were issued.
  [[nodiscard]] const Shown* WaitedPresent() const;

  FakePacerClock* clock_;
  double period_ns_;
  std::uint64_t anchor_ns_;
  std::vector<Shown> presents_;
  std::uint32_t late_presents_ = 0;
};

}  // namespace se::testing

#endif  // TESTS_SUPPORT_FAKE_PRESENT_PORT_H_
