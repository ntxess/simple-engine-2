// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/synthetic_clock.h"

#include <algorithm>
#include <cstdint>

#include "se_core/guid.h"
#include "se_core/rng.h"
#include "se_core/time.h"

namespace se {
namespace {

constexpr std::uint32_t kJitterStream = 0x51C0;
constexpr std::uint32_t kMissStream = 0x51C1;

SyntheticClockConfig MakeConfig(double hz, std::uint64_t seed,
                                double jitter_sec) {
  SyntheticClockConfig config;
  config.hz = hz;
  config.seed = seed;
  config.jitter_sec = jitter_sec;
  return config;
}

}  // namespace

SyntheticFrameClock::SyntheticFrameClock(double hz, std::uint64_t seed,
                                         double jitter_sec)
    : SyntheticFrameClock(MakeConfig(hz, seed, jitter_sec)) {}

SyntheticFrameClock::SyntheticFrameClock(const SyntheticClockConfig& config)
    : config_(config),
      period_ns_(1e9 / config.hz),
      jitter_ns_(
          std::clamp(config.jitter_sec * 1e9, 0.0, 0.9 * (1e9 / config.hz))) {}

FrameClock SyntheticFrameClock::Next() noexcept {
  std::uint32_t elapsed = 1;
  if (frame_ > 0 && config_.miss_probability > 0.0) {
    const auto miss = static_cast<double>(
        rng::Unit(rng::At(config_.seed, frame_, Guid{}, kMissStream, 0)));
    if (miss < config_.miss_probability) {
      elapsed +=
          1 + static_cast<std::uint32_t>(
                  rng::At(config_.seed, frame_, Guid{}, kMissStream, 1) & 1U);
    }
  }
  vblank_ += elapsed;
  missed_vblanks_ += elapsed - 1;
  const double jitter = static_cast<double>(rng::Unit(rng::At(
                            config_.seed, frame_, Guid{}, kJitterStream, 0))) *
                        jitter_ns_;
  const auto start = static_cast<std::uint64_t>(
      static_cast<double>(kOriginNs) +
      static_cast<double>(vblank_) * period_ns_ + jitter);
  FrameClock clock;
  clock.frame_start_ns = start;
  clock.prev_frame_start_ns = prev_start_ns_;
  clock.period_sec = 1.0 / config_.hz;
  clock.elapsed_vblanks = elapsed;
  clock.vblanks_known = config_.vblanks_known;
  clock.timer_p99_sec =
      config_.timer_p99_sec < 0.0 ? jitter_ns_ / 1e9 : config_.timer_p99_sec;
  clock.real_delta = prev_start_ns_ == 0
                         ? clock.period_sec
                         : static_cast<double>(start - prev_start_ns_) / 1e9;
  prev_start_ns_ = start;
  ++frame_;
  return clock;
}

}  // namespace se
