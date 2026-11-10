// Copyright 2026 The simple-engine-2 Authors.
//
// PacedClock: the dedicated sim thread's absolute-deadline scheduler (docs/
// design/dedicated-driver.md "Threads", sim, and "Parallel Work", CATCH-UP
// AND SPIRAL-OF-DEATH POLICY; ADR-084).
//
// Deadlines: L_k = T0 + (k - k0) * P + phi, where P = 1 s / (rate * scale)
// is the wall time per tick (exactly 1e9 / rate ns at scale 1, in integer
// arithmetic, so 120 ticks take exactly one second), T0 and k0 the anchor
// of the current grid, and phi the wake phase.
//   - WaitForDeadline(k): a timer to L_k - margin, margin = max(spin floor,
//     the measured timer overshoot's p99) capped at min(max_margin, P / 4),
//     then a spin with the pause instruction until L_k. It never returns
//     before L_k (unless stopped). The cap keeps a few long timer overshoots
//     (or a coarse timer) from turning the wait into a spin for most of the
//     period: the design spins only a short tail. Waits whose margin was cut
//     by the cap are counted (the timer overshoots more than the cap, so
//     those ticks may wake late; the SimThread warns once).
//   - DueTicks(next, now): the ticks whose deadlines have passed, at most
//     max_catch_up (8: 66.7 ms of sim time) per burst; the caller runs them
//     back to back, re-checks its commands and asks again. Behind by more
//     than dilation_after (250 ms: a stall, a suspension, a debugger break),
//     it stops chasing: T0 is re-based so the next tick is due now, the
//     dilation is counted, and tick numbers stay consecutive.
//   - phi: the platform proposes a target (SetPhaseTarget); it applies only
//     when the display period is a whole multiple of dt, and phi moves
//     towards it by at most phi_slew (0.1 ms) per tick (OnTickRun).
//   - Rebase (resume, time scale, dilation) moves the grid and bumps the
//     epoch, which the snapshot header carries: anchors of two epochs are
//     not on one grid.
//
// Time comes from a WaitClock: the system clock (MakeSystemWaitClock: se_os
// MonotonicNowNs and PreciseTimer) on the sim thread, or a ManualClock that
// drives it deterministically in tests. Systems never see either: only the
// driver reads the clock, and only for pacing.

#ifndef SE_SIM_PACED_CLOCK_H_
#define SE_SIM_PACED_CLOCK_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <utility>
#include <vector>

#include "se_core/sim_clock.h"
#include "se_core/types.h"

namespace se {

// The clock the sim thread paces itself with.
class WaitClock {
 public:
  virtual ~WaitClock() = default;
  // Nanoseconds on the engine's monotonic clock.
  [[nodiscard]] virtual std::uint64_t NowNs() noexcept = 0;
  // A coarse wait: returns at about |deadline_ns| (late by the timer's
  // overshoot, possibly a little early), or soon after |stop| is requested.
  virtual void SleepUntil(std::uint64_t deadline_ns,
                          const std::stop_token& stop) noexcept = 0;
  // One iteration of the final spin.
  virtual void Relax() noexcept = 0;
};

// The system clock: se_os MonotonicNowNs, a PreciseTimer created on the
// calling thread (so create it on the thread that waits), CpuRelax. Waits
// are cut into slices of at most 10 ms so stops are seen.
[[nodiscard]] std::unique_ptr<WaitClock> MakeSystemWaitClock();

// A clock that moves only when told: in tests the deadline grid, the
// overshoot accounting and whole sim-thread runs are deterministic.
//   kAutoAdvance  SleepUntil(d) moves the time to d plus the next scripted
//                 overshoot (SetOvershoots; may be negative: an early wake);
//                 Relax() moves it by relax_step_ns. A thread waiting on it
//                 runs as fast as it can, in simulated time.
//   kBlocking     the time moves only by Advance()/SetNow() from another
//                 thread; SleepUntil and Relax wait for it.
class ManualClock final : public WaitClock {
 public:
  enum class Mode : std::uint8_t { kAutoAdvance, kBlocking };

  explicit ManualClock(Mode mode = Mode::kAutoAdvance,
                       std::uint64_t start_ns = 1'000'000'000) noexcept
      : mode_(mode), now_(start_ns) {}

  [[nodiscard]] std::uint64_t NowNs() noexcept override {
    return now_.load(std::memory_order_acquire);
  }
  void SleepUntil(std::uint64_t deadline_ns,
                  const std::stop_token& stop) noexcept override;
  void Relax() noexcept override;

  // Any thread.
  void Advance(std::uint64_t ns) noexcept;
  void SetNow(std::uint64_t ns) noexcept;

  // kAutoAdvance: the overshoot of the k-th SleepUntil is
  // overshoots[k % size] (none when empty). Set before the clock is used.
  void SetOvershoots(std::vector<std::int64_t> overshoots) {
    overshoots_ = std::move(overshoots);
  }
  void SetRelaxStep(std::uint64_t ns) noexcept { relax_step_ns_ = ns; }

  [[nodiscard]] std::uint64_t Sleeps() const noexcept {
    return sleeps_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t Relaxes() const noexcept {
    return relaxes_.load(std::memory_order_relaxed);
  }

 private:
  void Bump() noexcept;

  const Mode mode_;
  std::atomic<std::uint64_t> now_;
  // Changes with every move of the time and on stop; waiters wait on it.
  std::atomic<std::uint32_t> moves_{0};
  std::vector<std::int64_t> overshoots_;
  std::uint64_t relax_step_ns_ = 1000;
  std::atomic<std::uint64_t> sleeps_{0};
  std::atomic<std::uint64_t> relaxes_{0};
};

struct PacedClockConfig {
  std::uint32_t tick_rate = kDefaultTickRate;
  std::uint32_t max_catch_up = 8;
  std::uint64_t dilation_after_ns = 250'000'000;
  // The spin tail's floor: the timer wakes at least this long before L_k.
  std::uint64_t spin_floor_ns = 300'000;
  // The spin tail's cap (with P / 4), however much the timer overshoots.
  std::uint64_t max_margin_ns = 2'000'000;
  // The most phi moves per tick.
  std::uint64_t phi_slew_ns = 100'000;
};

// The ticks DueTicks found due: first .. first + count - 1.
struct TickBurst {
  Tick first = 0;
  std::uint32_t count = 0;
  // T0 was re-based (dilation) before the burst.
  bool rebased = false;
  // How far behind the clock was when it re-based.
  std::uint64_t dilated_ns = 0;

  [[nodiscard]] Tick Last() const noexcept { return first + count - 1; }
};

struct PacedClockStats {
  std::uint64_t waits = 0;
  // Waits that slept on the timer (the others started inside the margin or
  // after the deadline).
  std::uint64_t timer_sleeps = 0;
  std::uint64_t spin_iterations = 0;
  // Waits that began after their deadline.
  std::uint64_t late_waits = 0;
  // Waits whose margin the cap cut (the overshoot's p99 was above it).
  std::uint64_t capped_waits = 0;
  std::uint64_t dilations = 0;
  std::uint64_t dilated_ns = 0;
  std::uint64_t rebases = 0;
  // Timer wake minus timer target, as far as it was late, the last sample
  // and the largest.
  std::uint64_t last_overshoot_ns = 0;
  std::uint64_t max_overshoot_ns = 0;
  // Return of WaitForDeadline minus L_k: the last and the largest.
  std::uint64_t last_wake_error_ns = 0;
  std::uint64_t max_wake_error_ns = 0;
};

class PacedClock {
 public:
  // Samples kept for the overshoot's p99 and the wake error's.
  static constexpr std::size_t kSamples = 256;

  PacedClock(WaitClock& clock, const PacedClockConfig& config);

  // Anchors tick |next| at |t0_ns| (L_next = t0_ns + phi): a session start.
  void Start(Tick next, std::uint64_t t0_ns) noexcept;
  // Moves the grid so that L_next = t0_ns + phi, and bumps the epoch.
  void Rebase(Tick next, std::uint64_t t0_ns) noexcept;

  // L_k. |k| must not be before the grid's anchor tick.
  [[nodiscard]] std::uint64_t DeadlineNs(Tick k) const noexcept;
  // Wall nanoseconds per tick (dt / scale), rounded.
  [[nodiscard]] std::uint64_t PeriodNs() const noexcept;

  // Waits for L_k (see the file comment); returns the clock's time then.
  std::uint64_t WaitForDeadline(Tick k, const std::stop_token& stop) noexcept;
  // The ticks due from |next| at |now_ns| (may re-base: dilation).
  [[nodiscard]] TickBurst DueTicks(Tick next, std::uint64_t now_ns) noexcept;

  // Time scale: the wall time per tick becomes dt / scale (never dt). The
  // grid continues from tick |next|'s current deadline. scale must be
  // positive and finite.
  void SetTimeScale(double scale, Tick next) noexcept;
  // The platform's wake phase target; ignored unless display_multiple.
  void SetPhaseTarget(std::int64_t phi_ns, bool display_multiple) noexcept;
  // After each tick: phi moves towards its target by at most phi_slew.
  void OnTickRun() noexcept;

  [[nodiscard]] std::uint32_t TickRate() const noexcept { return rate_; }
  [[nodiscard]] double TimeScale() const noexcept { return scale_; }
  [[nodiscard]] std::int64_t Phi() const noexcept { return phi_; }
  [[nodiscard]] std::int64_t PhiTarget() const noexcept { return phi_target_; }
  [[nodiscard]] std::uint32_t Epoch() const noexcept { return epoch_; }
  // max(spin floor, overshoot p99), at most MarginCapNs().
  [[nodiscard]] std::uint64_t MarginNs() const noexcept;
  // max(spin floor, min(max_margin, P / 4)).
  [[nodiscard]] std::uint64_t MarginCapNs() const noexcept;
  [[nodiscard]] std::uint64_t OvershootP99Ns() const noexcept {
    return overshoot_p99_;
  }
  [[nodiscard]] std::uint64_t WakeErrorP99Ns() const noexcept;
  [[nodiscard]] const PacedClockStats& Stats() const noexcept { return stats_; }
  [[nodiscard]] WaitClock& Clock() noexcept { return *clock_; }

 private:
  void RecordOvershoot(std::uint64_t ns) noexcept;

  WaitClock* clock_;
  PacedClockConfig config_;
  std::uint32_t rate_;
  double scale_ = 1.0;
  // The grid: L_k = t0_ + offset(k - k0_) + phi_.
  std::uint64_t t0_ = 0;
  Tick k0_ = 1;
  std::int64_t phi_ = 0;
  std::int64_t phi_target_ = 0;
  bool phase_locked_ = false;
  std::uint32_t epoch_ = 0;
  std::array<std::uint64_t, kSamples> overshoots_{};
  std::size_t overshoot_count_ = 0;
  std::uint64_t overshoot_p99_ = 0;
  std::array<std::uint64_t, kSamples> wake_errors_{};
  std::size_t wake_error_count_ = 0;
  PacedClockStats stats_;
};

}  // namespace se

#endif  // SE_SIM_PACED_CLOCK_H_
