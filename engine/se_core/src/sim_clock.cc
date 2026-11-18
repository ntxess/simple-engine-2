// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/sim_clock.h"

#include <cstdint>

namespace se {

Tick TicksFromSeconds(double seconds, std::uint32_t rate) noexcept {
  constexpr double kSnap = 1e-6;
  const double ticks = seconds * rate;
  if (!(ticks > kSnap)) return 0;
  const auto whole = static_cast<Tick>(ticks);
  const double fraction = ticks - static_cast<double>(whole);
  return fraction <= kSnap ? whole : whole + 1;
}

}  // namespace se
