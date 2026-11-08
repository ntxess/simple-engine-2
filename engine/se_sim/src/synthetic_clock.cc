// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/synthetic_clock.h"

#include <algorithm>
#include <cstdint>

#include "se_core/guid.h"
#include "se_core/rng.h"
#include "se_core/time.h"

namespace se {
namespace {

// Frame 0 starts one second in, so no start is 0 ("no previous frame").
constexpr double kOriginNs = 1e9;
constexpr std::uint32_t kJitterStream = 0x51C0;

}  // namespace

SyntheticFrameClock::SyntheticFrameClock(double hz, std::uint64_t seed,
                                         double jitter_sec)
    : hz_(hz),
      period_ns_(1e9 / hz),
      seed_(seed),
      jitter_ns_(std::clamp(jitter_sec * 1e9, 0.0, 0.9 * (1e9 / hz))) {}

FrameClock SyntheticFrameClock::Next() noexcept {
  const double jitter = static_cast<double>(rng::Unit(
                            rng::At(seed_, frame_, Guid{}, kJitterStream, 0))) *
                        jitter_ns_;
  const auto start = static_cast<std::uint64_t>(
      kOriginNs + static_cast<double>(frame_) * period_ns_ + jitter);
  FrameClock clock;
  clock.frame_start_ns = start;
  clock.prev_frame_start_ns = prev_start_ns_;
  clock.period_sec = 1.0 / hz_;
  clock.elapsed_vblanks = 1;
  clock.vblanks_known = true;
  clock.timer_p99_sec = jitter_ns_ / 1e9;
  clock.real_delta = prev_start_ns_ == 0
                         ? clock.period_sec
                         : static_cast<double>(start - prev_start_ns_) / 1e9;
  prev_start_ns_ = start;
  ++frame_;
  return clock;
}

}  // namespace se
