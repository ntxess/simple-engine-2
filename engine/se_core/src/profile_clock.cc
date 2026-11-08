// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/profile_clock.h"

#include <chrono>
#include <cstdint>

namespace se {

std::uint64_t ProfileNowNs() noexcept {
  using Clock = std::chrono::steady_clock;  // se-allow(clock): profiling only
  const auto now = Clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

}  // namespace se
