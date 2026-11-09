// Copyright 2026 The simple-engine-2 Authors.

#include "support/fake_present_port.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "se_platform/vblank_sampler.h"
#include "se_runtime/present_port.h"

namespace se::testing {

void FakePacerClock::SleepNs(std::uint64_t ns) noexcept {
  ++sleep_count;
  last_sleep_ns = ns;
  now_ns += ns + late_ns;
}

FakePresentPort::FakePresentPort(FakePacerClock* clock, double period_ns)
    : clock_(clock), period_ns_(period_ns), anchor_ns_(clock->now_ns) {}

std::uint64_t FakePresentPort::RefreshAt(std::uint64_t time_ns) const {
  if (time_ns < anchor_ns_) return 0;
  // Vblank times are rounded to whole nanoseconds (and grid arithmetic may
  // add one more): 2 ns of slack keep a vblank on its own refresh.
  return static_cast<std::uint64_t>(std::floor(
      (static_cast<double>(time_ns - anchor_ns_) + 2.0) / period_ns_));
}

std::uint64_t FakePresentPort::VblankAt(std::uint64_t refresh) const {
  return anchor_ns_ + static_cast<std::uint64_t>(std::llround(
                          static_cast<double>(refresh) * period_ns_));
}

std::uint64_t FakePresentPort::LatestVblank(std::uint64_t now_ns) const {
  const std::uint64_t refresh = RefreshAt(now_ns);
  std::uint64_t vblank = VblankAt(refresh);
  if (vblank > now_ns && refresh > 0) vblank = VblankAt(refresh - 1);
  return vblank;
}

VblankReading FakePresentPort::Reading(std::uint64_t now_ns,
                                       double scale) const {
  VblankReading reading;
  reading.running = true;
  reading.resolved = true;
  reading.fresh = true;
  reading.valid = true;
  reading.last_vblank_ns = LatestVblank(now_ns);
  reading.count = RefreshAt(now_ns);
  reading.period_ns =
      static_cast<std::uint64_t>(std::llround(period_ns_ * scale));
  reading.spread_ns = reading.period_ns / 100;
  static char monitor = 0;
  reading.monitor = &monitor;
  return reading;
}

std::optional<std::uint64_t> FakePresentPort::ShownRefresh(
    std::uint64_t id) const {
  if (id == 0 || id > presents_.size()) return std::nullopt;
  return presents_[id - 1].refresh + kRefreshBase;
}

void FakePresentPort::Present() {
  const std::uint64_t now = clock_->now_ns;
  std::uint64_t refresh = RefreshAt(now) + 1;
  if (!presents_.empty()) {
    refresh = std::max(refresh, presents_.back().refresh + 1);
  }
  if (late_presents_ > 0) {
    --late_presents_;
    ++refresh;
  }
  presents_.push_back(
      Shown{static_cast<std::uint64_t>(presents_.size()) + 1, refresh});
}

bool FakePresentPort::GetPresentStats(PresentStatistics& stats) {
  if (!caps.present_stats) return false;
  const std::uint64_t now = clock_->now_ns;
  const Shown* shown = nullptr;
  for (const Shown& present : presents_) {
    if (VblankAt(present.refresh) <= now) shown = &present;
  }
  if (shown == nullptr) return false;
  stats.present_count = shown->id;
  stats.present_refresh_count = shown->refresh + kRefreshBase;
  const std::uint64_t sync = RefreshAt(now);
  stats.sync_refresh_count = sync + kRefreshBase;
  stats.sync_time_ns = VblankAt(sync);
  stats.last_present_count = presents_.size();
  return true;
}

void FakePresentPort::WaitForNewestShown() {
  if (presents_.empty()) return;
  const std::uint64_t shown =
      VblankAt(presents_.back().refresh) + wake_latency_ns;
  clock_->now_ns = std::max(clock_->now_ns, shown);
}

bool FakePresentPort::NewestShown() const {
  return presents_.empty() ||
         VblankAt(presents_.back().refresh) + wake_latency_ns <= clock_->now_ns;
}

bool FakePresentPort::SetFrameLatency(int frames) {
  latencies.push_back(frames);
  return caps.frame_latency_waitable;
}

PresentWait FakePresentPort::WaitFrameLatency(std::uint64_t timeout_ns) {
  if (!caps.frame_latency_waitable) return PresentWait::kUnsupported;
  if (timeout_ns == 0) {
    ++waitable_polls;
    return NewestShown() ? PresentWait::kReady : PresentWait::kTimeout;
  }
  ++waitable_waits;
  WaitForNewestShown();
  return PresentWait::kReady;
}

bool FakePresentPort::SetPresentMode(SwapchainMode mode) {
  present_modes.push_back(mode);
  return present_mode_result;
}

void FakePresentPort::OnFrameSubmitted(
    [[maybe_unused]] SDL_GPUFence* fence) noexcept {
  ++submitted;
}

PresentWait FakePresentPort::WaitFrameFence([[maybe_unused]] int frames_back,
                                            std::uint64_t timeout_ns) {
  if (timeout_ns == 0) {
    ++fence_polls;
    return fence_pending ? PresentWait::kTimeout : PresentWait::kReady;
  }
  ++fence_waits;
  fence_pending = false;
  clock_->now_ns += 500'000;
  return PresentWait::kReady;
}

bool FakePresentPort::RepresentLast() {
  ++represents;
  Present();
  return true;
}

bool FakePresentPort::WaitSwapchain() {
  ++swapchain_waits;
  WaitForNewestShown();
  return true;
}

}  // namespace se::testing
