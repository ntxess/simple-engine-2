// Copyright 2026 The simple-engine-2 Authors.
//
// Simulation time. Systems read time only from a World's SimClock: the tick
// number and the fixed step dt in seconds. Wall time never reaches a tick.

#ifndef SE_CORE_SIM_CLOCK_H_
#define SE_CORE_SIM_CLOCK_H_

#include <cstdint>

#include "se_core/types.h"

namespace se {

inline constexpr std::uint32_t kDefaultTickRate = 120;

struct SimClock {
  // The tick being computed during a Schedule step, or the last completed
  // tick between steps. 0 is the state after loading.
  Tick tick = 0;
  std::uint32_t rate = kDefaultTickRate;
  double dt = 1.0 / kDefaultTickRate;

  // Simulated seconds since tick 0.
  [[nodiscard]] double Time() const noexcept {
    return static_cast<double>(tick) * dt;
  }

  void Advance() noexcept { ++tick; }

  // Changes the tick rate; applies between ticks.
  void SetRate(std::uint32_t new_rate) noexcept {
    rate = new_rate;
    dt = 1.0 / new_rate;
  }
};

// The written rule for durations given in seconds: the number of ticks is
// ceil(seconds * rate), where products within 1e-6 of an integer count as
// that integer (so 0.1 s at 120 Hz is 12 ticks, not 13). Negative durations
// are 0 ticks.
[[nodiscard]] Tick TicksFromSeconds(double seconds,
                                    std::uint32_t rate) noexcept;

// The same rule for integer milliseconds, computed exactly:
// ceil(ms * rate / 1000).
[[nodiscard]] constexpr Tick TicksFromMilliseconds(
    std::uint64_t ms, std::uint32_t rate) noexcept {
  return (ms * rate + 999) / 1000;
}

}  // namespace se

#endif  // SE_CORE_SIM_CLOCK_H_
