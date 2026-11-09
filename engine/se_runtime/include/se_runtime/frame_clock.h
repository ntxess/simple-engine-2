// Copyright 2026 The simple-engine-2 Authors.
//
// The FrameClock of each iterate in M5 (docs/milestone-specs.md M5, "The
// frame"): frame starts from SDL_GetTicksNS and the presentation period
// from SDL's display mode, with vblank counts unknown, so the FixedStepper
// rounds deltas. M6's FramePacer replaces it with vblank-grid times from
// present statistics (ADR-016).
//
// With --fixed-frame-dt the clock is synthetic instead: every frame is
// exactly that long and one period, whatever the wall clock says, so a run
// ticks deterministically (tests, captures).
//
// Reset() makes the next frame a first frame: the stepper then counts one
// period, so leaving Suspended runs no catch-up ticks.

#ifndef SE_RUNTIME_FRAME_CLOCK_H_
#define SE_RUNTIME_FRAME_CLOCK_H_

#include <cstdint>

#include "se_core/time.h"

namespace se {

class FrameClockSource {
 public:
  // The period assumed when SDL does not know the refresh rate.
  static constexpr double kFallbackHz = 60.0;
  // SDL_GetTicksNS is precise; the stepper snaps deltas within this plus
  // its own margin.
  static constexpr double kTimerP99Sec = 0.5e-3;

  // |fixed_frame_dt_sec| > 0 makes the clock synthetic.
  explicit FrameClockSource(double fixed_frame_dt_sec = 0.0) noexcept
      : fixed_dt_sec_(fixed_frame_dt_sec) {}

  // The clock of a frame starting at |now_ns| (SDL_GetTicksNS) on a display
  // at |refresh_hz| (0 when unknown).
  [[nodiscard]] FrameClock Next(std::uint64_t now_ns, double refresh_hz,
                                bool modal) noexcept;

  // The next frame is a first frame (no previous start).
  void Reset() noexcept { prev_start_ns_ = 0; }

  [[nodiscard]] bool Synthetic() const noexcept { return fixed_dt_sec_ > 0.0; }
  [[nodiscard]] std::uint64_t Frames() const noexcept { return frames_; }
  [[nodiscard]] std::uint32_t Resets() const noexcept { return resets_; }

 private:
  double fixed_dt_sec_ = 0.0;
  std::uint64_t prev_start_ns_ = 0;
  // The synthetic clock's time; starts at the first real frame start.
  std::uint64_t synthetic_ns_ = 0;
  std::uint64_t frames_ = 0;
  std::uint32_t resets_ = 0;
};

}  // namespace se

#endif  // SE_RUNTIME_FRAME_CLOCK_H_
