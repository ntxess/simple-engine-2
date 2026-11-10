// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/paced_clock.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <thread>

#include "se_core/assert.h"

namespace se {
namespace {

constexpr std::uint64_t kNsPerSecond = 1'000'000'000;

// The p99 of the first |count| samples (the newest kSamples).
std::uint64_t P99(const std::array<std::uint64_t, PacedClock::kSamples>& ring,
                  std::size_t count) noexcept {
  const std::size_t n = std::min(count, ring.size());
  if (n == 0) return 0;
  std::array<std::uint64_t, PacedClock::kSamples> sorted = ring;
  // The smallest value at or above 99% of the samples.
  const std::size_t at = (n * 99 + 99) / 100 - 1;
  std::nth_element(sorted.begin(),
                   sorted.begin() + static_cast<std::ptrdiff_t>(at),
                   sorted.begin() + static_cast<std::ptrdiff_t>(n));
  return sorted[at];
}

}  // namespace

void ManualClock::SleepUntil(std::uint64_t deadline_ns,
                             const std::stop_token& stop) noexcept {
  const std::uint64_t k = sleeps_.fetch_add(1, std::memory_order_relaxed);
  if (mode_ == Mode::kAutoAdvance) {
    std::int64_t overshoot = 0;
    if (!overshoots_.empty()) overshoot = overshoots_[k % overshoots_.size()];
    const std::uint64_t target =
        overshoot >= 0 ? deadline_ns + static_cast<std::uint64_t>(overshoot)
                       : deadline_ns - static_cast<std::uint64_t>(-overshoot);
    std::uint64_t now = now_.load(std::memory_order_relaxed);
    while (now < target && !now_.compare_exchange_weak(
                               now, target, std::memory_order_acq_rel)) {
    }
    Bump();
    return;
  }
  const std::stop_callback wake_on_stop(stop, [this]() noexcept { Bump(); });
  for (;;) {
    const std::uint32_t seen = moves_.load(std::memory_order_acquire);
    if (now_.load(std::memory_order_acquire) >= deadline_ns ||
        stop.stop_requested()) {
      return;
    }
    moves_.wait(seen, std::memory_order_acquire);
  }
}

void ManualClock::Relax() noexcept {
  relaxes_.fetch_add(1, std::memory_order_relaxed);
  if (mode_ == Mode::kAutoAdvance) {
    now_.fetch_add(relax_step_ns_, std::memory_order_acq_rel);
    Bump();
  } else {
    std::this_thread::yield();
  }
}

void ManualClock::Advance(std::uint64_t ns) noexcept {
  now_.fetch_add(ns, std::memory_order_acq_rel);
  Bump();
}

void ManualClock::SetNow(std::uint64_t ns) noexcept {
  now_.store(ns, std::memory_order_release);
  Bump();
}

void ManualClock::Bump() noexcept {
  moves_.fetch_add(1, std::memory_order_release);
  moves_.notify_all();
}

PacedClock::PacedClock(WaitClock& clock, const PacedClockConfig& config)
    : clock_(&clock),
      config_(config),
      rate_(config.tick_rate == 0 ? kDefaultTickRate : config.tick_rate) {
  SE_ASSERT_MSG(config_.max_catch_up >= 1, "a burst runs at least one tick");
}

void PacedClock::Start(Tick next, std::uint64_t t0_ns) noexcept {
  Rebase(next, t0_ns);
}

void PacedClock::Rebase(Tick next, std::uint64_t t0_ns) noexcept {
  t0_ = t0_ns;
  k0_ = next;
  ++epoch_;
  ++stats_.rebases;
}

std::uint64_t PacedClock::DeadlineNs(Tick k) const noexcept {
  SE_ASSERT_MSG(k >= k0_, "a deadline before the grid's anchor tick");
  const std::uint64_t n = k - k0_;
  std::uint64_t offset = 0;
  if (scale_ == 1.0) {
    // Exact: rate ticks take one second.
    offset = n * kNsPerSecond / rate_;
  } else {
    offset = static_cast<std::uint64_t>(std::llround(
        static_cast<double>(n) * static_cast<double>(kNsPerSecond) /
        (static_cast<double>(rate_) * scale_)));
  }
  const std::uint64_t base = t0_ + offset;
  return phi_ >= 0 ? base + static_cast<std::uint64_t>(phi_)
                   : base - static_cast<std::uint64_t>(-phi_);
}

std::uint64_t PacedClock::PeriodNs() const noexcept {
  return static_cast<std::uint64_t>(
      std::llround(static_cast<double>(kNsPerSecond) /
                   (static_cast<double>(rate_) * scale_)));
}

std::uint64_t PacedClock::MarginCapNs() const noexcept {
  return std::max(config_.spin_floor_ns,
                  std::min(config_.max_margin_ns, PeriodNs() / 4));
}

std::uint64_t PacedClock::MarginNs() const noexcept {
  return std::min(std::max(config_.spin_floor_ns, overshoot_p99_),
                  MarginCapNs());
}

std::uint64_t PacedClock::WakeErrorP99Ns() const noexcept {
  return P99(wake_errors_, wake_error_count_);
}

void PacedClock::RecordOvershoot(std::uint64_t ns) noexcept {
  overshoots_[overshoot_count_ % kSamples] = ns;
  ++overshoot_count_;
  overshoot_p99_ = P99(overshoots_, overshoot_count_);
  stats_.last_overshoot_ns = ns;
  stats_.max_overshoot_ns = std::max(stats_.max_overshoot_ns, ns);
}

std::uint64_t PacedClock::WaitForDeadline(
    Tick k, const std::stop_token& stop) noexcept {
  ++stats_.waits;
  const std::uint64_t deadline = DeadlineNs(k);
  std::uint64_t now = clock_->NowNs();
  if (now >= deadline) {
    ++stats_.late_waits;
  } else {
    const std::uint64_t margin = MarginNs();
    if (overshoot_p99_ > margin) ++stats_.capped_waits;
    if (deadline - now > margin) {
      const std::uint64_t target = deadline - margin;
      clock_->SleepUntil(target, stop);
      ++stats_.timer_sleeps;
      now = clock_->NowNs();
      RecordOvershoot(now > target ? now - target : 0);
    }
    while (now < deadline) {
      if (stop.stop_requested()) return now;
      clock_->Relax();
      ++stats_.spin_iterations;
      now = clock_->NowNs();
    }
  }
  const std::uint64_t error = now - deadline;
  wake_errors_[wake_error_count_ % kSamples] = error;
  ++wake_error_count_;
  stats_.last_wake_error_ns = error;
  stats_.max_wake_error_ns = std::max(stats_.max_wake_error_ns, error);
  return now;
}

TickBurst PacedClock::DueTicks(Tick next, std::uint64_t now_ns) noexcept {
  TickBurst burst;
  burst.first = next;
  const std::uint64_t first = DeadlineNs(next);
  if (now_ns < first) return burst;
  const std::uint64_t behind = now_ns - first;
  if (behind > config_.dilation_after_ns) {
    // Stop chasing wall time: the next tick is due now, on a new grid.
    ++stats_.dilations;
    stats_.dilated_ns += behind;
    const std::uint64_t t0 = phi_ >= 0
                                 ? now_ns - static_cast<std::uint64_t>(phi_)
                                 : now_ns + static_cast<std::uint64_t>(-phi_);
    Rebase(next, t0);
    burst.count = 1;
    burst.rebased = true;
    burst.dilated_ns = behind;
    return burst;
  }
  burst.count = 1;
  while (burst.count < config_.max_catch_up &&
         DeadlineNs(next + burst.count) <= now_ns) {
    ++burst.count;
  }
  return burst;
}

void PacedClock::SetTimeScale(double scale, Tick next) noexcept {
  SE_ASSERT_MSG(scale > 0.0 && std::isfinite(scale),
                "the time scale must be positive and finite");
  // Tick |next| keeps its deadline; the ticks after it follow the new
  // period. The grid is continuous, so the epoch stays.
  const std::uint64_t keep = DeadlineNs(next);
  scale_ = scale;
  k0_ = next;
  t0_ = phi_ >= 0 ? keep - static_cast<std::uint64_t>(phi_)
                  : keep + static_cast<std::uint64_t>(-phi_);
}

void PacedClock::SetPhaseTarget(std::int64_t phi_ns,
                                bool display_multiple) noexcept {
  phase_locked_ = display_multiple;
  if (display_multiple) phi_target_ = phi_ns;
}

void PacedClock::OnTickRun() noexcept {
  if (!phase_locked_ || phi_ == phi_target_) return;
  const auto slew = static_cast<std::int64_t>(config_.phi_slew_ns);
  const std::int64_t step = std::clamp(phi_target_ - phi_, -slew, slew);
  phi_ += step;
}

}  // namespace se
