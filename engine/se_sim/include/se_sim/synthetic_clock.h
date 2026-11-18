// Copyright 2026 The simple-engine-2 Authors.
//
// SyntheticFrameClock: FrameClocks for a display at a fixed rate, for
// headless runs (--fixed-frame-dt) and the frame-clock independence tests.
//
// Vblank v is at kOriginNs + v * T. Frame n targets vblank v_n: v_0 = 1,
// and each later frame is one vblank after the previous one, or two or
// three when it misses (seeded, with miss_probability). A frame starts at
// its vblank plus a jitter drawn uniformly from [0, jitter_sec), at most
// 0.9 T. The first frame has no previous start, which the stepper counts as
// one period, so at every rate the first tick's window starts at kOriginNs
// and, on a regular clock, tick k's window ends within the jitter of
// kOriginNs + k * dt (tests/support/stamped_input.h relies on this).
//
// By default frames report the vblanks elapsed and a timer p99 equal to the
// jitter, so Free-cadence deltas snap to the period. A lower reported p99
// stops the snapping, and unknown vblank counts make the stepper round the
// delta instead: the irregular clocks of the determinism tests.

#ifndef SE_SIM_SYNTHETIC_CLOCK_H_
#define SE_SIM_SYNTHETIC_CLOCK_H_

#include <cstdint>

#include "se_core/time.h"

namespace se {

struct SyntheticClockConfig {
  double hz = 60.0;
  std::uint64_t seed = 0;
  // Start jitter: uniform in [0, jitter_sec), at most 0.9 periods.
  double jitter_sec = 0.0;
  // Probability that a frame (after the first) misses one or two vblanks.
  double miss_probability = 0.0;
  // Whether frames report how many vblanks elapsed.
  bool vblanks_known = true;
  // The timer p99 the frames report; negative reports the jitter.
  double timer_p99_sec = -1.0;
};

class SyntheticFrameClock {
 public:
  // Vblank 0; the first frame starts one period later.
  static constexpr std::uint64_t kOriginNs = 1'000'000'000;

  SyntheticFrameClock(double hz, std::uint64_t seed, double jitter_sec);
  explicit SyntheticFrameClock(const SyntheticClockConfig& config);

  // The next frame's clock.
  [[nodiscard]] FrameClock Next() noexcept;

  [[nodiscard]] double Hz() const noexcept { return config_.hz; }
  [[nodiscard]] std::uint64_t Frames() const noexcept { return frame_; }
  // Vblanks missed so far.
  [[nodiscard]] std::uint64_t MissedVblanks() const noexcept {
    return missed_vblanks_;
  }

 private:
  SyntheticClockConfig config_;
  double period_ns_;
  double jitter_ns_;
  std::uint64_t frame_ = 0;
  std::uint64_t vblank_ = 0;
  std::uint64_t missed_vblanks_ = 0;
  std::uint64_t prev_start_ns_ = 0;
};

}  // namespace se

#endif  // SE_SIM_SYNTHETIC_CLOCK_H_
