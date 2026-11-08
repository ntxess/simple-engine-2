// Copyright 2026 The simple-engine-2 Authors.
//
// SyntheticFrameClock: FrameClocks for a display at a fixed rate with
// seeded start jitter, for headless runs (--fixed-frame-dt) and the
// frame-clock independence tests. Frame n starts at vblank n plus a jitter
// drawn uniformly from [0, jitter_sec); jitter below one period keeps one
// elapsed vblank per frame.

#ifndef SE_SIM_SYNTHETIC_CLOCK_H_
#define SE_SIM_SYNTHETIC_CLOCK_H_

#include <cstdint>

#include "se_core/time.h"

namespace se {

class SyntheticFrameClock {
 public:
  SyntheticFrameClock(double hz, std::uint64_t seed, double jitter_sec);

  // The next frame's clock.
  [[nodiscard]] FrameClock Next() noexcept;

  [[nodiscard]] double Hz() const noexcept { return hz_; }
  [[nodiscard]] std::uint64_t Frames() const noexcept { return frame_; }

 private:
  double hz_;
  double period_ns_;
  std::uint64_t seed_;
  double jitter_ns_;
  std::uint64_t frame_ = 0;
  std::uint64_t prev_start_ns_ = 0;
};

}  // namespace se

#endif  // SE_SIM_SYNTHETIC_CLOCK_H_
