// Copyright 2026 The simple-engine-2 Authors.

#include "se_audio/pcm_clock.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace se::audio {

void PcmClock::Calibrate(std::uint64_t now_ns, std::uint64_t frames) noexcept {
  if (count_ > 0 && frames < last_frames_) {
    // The engine's clock went back: a new engine or device.
    count_ = 0;
    next_ = 0;
  }
  last_frames_ = frames;
  offsets_[next_] =
      static_cast<double>(frames) -
      static_cast<double>(now_ns) * static_cast<double>(rate_) * 1e-9;
  next_ = (next_ + 1) % kWindow;
  count_ = std::min(count_ + 1, kWindow);
}

std::uint64_t PcmClock::FrameAt(std::uint64_t ns) const noexcept {
  if (count_ == 0) return 0;
  double offset = offsets_[0];
  for (std::size_t i = 1; i < count_; ++i) {
    offset = std::max(offset, offsets_[i]);
  }
  const double frame =
      static_cast<double>(ns) * static_cast<double>(rate_) * 1e-9 + offset;
  return frame <= 0.0 ? 0 : static_cast<std::uint64_t>(std::llround(frame));
}

std::uint64_t PcmClock::FramesFor(std::uint64_t ns) const noexcept {
  return static_cast<std::uint64_t>(std::llround(
      static_cast<double>(ns) * static_cast<double>(rate_) * 1e-9));
}

}  // namespace se::audio
