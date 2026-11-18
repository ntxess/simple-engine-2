// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/edge_placement.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace se {

int PlaceEdge(std::uint64_t stamp_ns,
              std::span<const std::uint64_t> window_ends) noexcept {
  if (window_ends.empty()) return kEdgeWaits;
  const std::size_t last = window_ends.size() - 1;
  for (std::size_t i = 0; i < last; ++i) {
    if (stamp_ns < window_ends[i]) return static_cast<int>(i);
  }
  return static_cast<int>(last);
}

void PlaceEdges(std::span<const std::uint64_t> stamps,
                std::span<const std::uint64_t> window_ends,
                std::span<int> out) noexcept {
  const std::size_t n = std::min(stamps.size(), out.size());
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = PlaceEdge(stamps[i], window_ends);
  }
}

void LockedTickWindows(std::uint64_t prev_start_ns, std::uint64_t start_ns,
                       std::span<std::uint64_t> out) noexcept {
  const std::uint64_t steps = out.size();
  const std::uint64_t span =
      start_ns > prev_start_ns ? start_ns - prev_start_ns : 0;
  for (std::uint64_t i = 0; i < steps; ++i) {
    out[i] = prev_start_ns + span * (i + 1) / steps;
  }
}

void FreeTickWindows(std::uint64_t start_ns, std::uint64_t remainder_ns,
                     std::uint32_t tick_rate,
                     std::span<std::uint64_t> out) noexcept {
  const std::uint64_t steps = out.size();
  const std::uint64_t rate = tick_rate == 0 ? 120 : tick_rate;
  for (std::uint64_t i = 0; i < steps; ++i) {
    const std::uint64_t back =
        remainder_ns + (steps - 1 - i) * 1'000'000'000ULL / rate;
    out[i] = back < start_ns ? start_ns - back : 0;
  }
}

}  // namespace se
