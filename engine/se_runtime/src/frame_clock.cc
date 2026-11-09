// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/frame_clock.h"

#include <cmath>
#include <cstdint>

#include "se_core/time.h"

namespace se {

FrameClock FrameClockSource::Next(std::uint64_t now_ns, double refresh_hz,
                                  bool modal) noexcept {
  FrameClock clock;
  clock.modal = modal;
  clock.timer_p99_sec = kTimerP99Sec;
  if (prev_start_ns_ == 0 && frames_ > 0) ++resets_;
  ++frames_;
  if (Synthetic()) {
    const auto dt_ns =
        static_cast<std::uint64_t>(std::llround(fixed_dt_sec_ * 1e9));
    // The first frame starts on the wall clock; later ones exactly dt_ns
    // apart. A reset keeps the time and drops the previous start.
    synthetic_ns_ = synthetic_ns_ == 0 ? now_ns : synthetic_ns_ + dt_ns;
    clock.frame_start_ns = synthetic_ns_;
    clock.period_sec = fixed_dt_sec_;
    clock.elapsed_vblanks = 1;
    clock.vblanks_known = true;
  } else {
    clock.frame_start_ns = now_ns;
    clock.period_sec = 1.0 / (refresh_hz > 1.0 ? refresh_hz : kFallbackHz);
    clock.vblanks_known = false;
  }
  clock.prev_frame_start_ns = prev_start_ns_;
  clock.real_delta =
      prev_start_ns_ != 0 && clock.frame_start_ns > prev_start_ns_
          ? static_cast<double>(clock.frame_start_ns - prev_start_ns_) * 1e-9
          : clock.period_sec;
  prev_start_ns_ = clock.frame_start_ns;
  return clock;
}

}  // namespace se
