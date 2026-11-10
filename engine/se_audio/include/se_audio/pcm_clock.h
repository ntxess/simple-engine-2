// Copyright 2026 The simple-engine-2 Authors.
//
// PcmClock: maps the platform clock (SDL_GetTicksNS) to the audio engine's
// clock, the PCM frames it has mixed (docs/milestone-specs.md M8; ADR-077).
// Pure: the caller reads both clocks and calibrates once per frame.
//
// A device mixes in periods (10 ms with WASAPI's shared mode), so the
// engine's frame count is a staircase in time. Each calibration gives the
// offset (frames - ns * rate / 1e9); the mapping uses the largest offset of
// the last kWindow calibrations, which is the staircase's leading edge: the
// frame the mixer starts with at that moment. Spacing between two mapped
// times is exact (rate * difference); only the absolute position follows
// the device. A frame count that goes backwards (a new engine) starts over.

#ifndef SE_AUDIO_PCM_CLOCK_H_
#define SE_AUDIO_PCM_CLOCK_H_

#include <array>
#include <cstddef>
#include <cstdint>

namespace se::audio {

class PcmClock {
 public:
  static constexpr std::size_t kWindow = 64;

  explicit PcmClock(std::uint32_t sample_rate) noexcept
      : rate_(sample_rate == 0 ? 48000 : sample_rate) {}

  // The engine had mixed |frames| frames at platform time |now_ns|.
  void Calibrate(std::uint64_t now_ns, std::uint64_t frames) noexcept;
  [[nodiscard]] bool Calibrated() const noexcept { return count_ > 0; }

  // The engine frame mixed at platform time |ns| (0 before calibration or
  // before the engine's start).
  [[nodiscard]] std::uint64_t FrameAt(std::uint64_t ns) const noexcept;
  // Frames for |ns| nanoseconds at the clock's rate (rounded).
  [[nodiscard]] std::uint64_t FramesFor(std::uint64_t ns) const noexcept;
  [[nodiscard]] std::uint32_t SampleRate() const noexcept { return rate_; }

 private:
  std::uint32_t rate_;
  std::array<double, kWindow> offsets_{};
  std::size_t count_ = 0;
  std::size_t next_ = 0;
  std::uint64_t last_frames_ = 0;
};

}  // namespace se::audio

#endif  // SE_AUDIO_PCM_CLOCK_H_
