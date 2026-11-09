// Copyright 2026 The simple-engine-2 Authors.
//
// Placing input edges on ticks (docs/design/mt-design.md "Input And
// Latency", "Placing edges on ticks"; docs/milestone-specs.md M8). Pure:
// no clock reads, no state.
//
// Each tick of a frame owns an input window that ends at its window end
// (StepPlan::tick_window_end_ns). The windows of one frame are:
//
//   Locked cadence  the k ticks split the wall time since the previous
//                   frame start into k equal windows:
//                   end_i = prev_start + span * (i + 1) / k,
//                   span = start - prev_start;
//   Free cadence    tick i's window ends at
//                   start - remainder - (steps - 1 - i) * dt,
//                   remainder being the accumulator left after the frame's
//                   steps (debt included).
//
// FixedStepper::Plan computes StepPlan::tick_window_end_ns with
// LockedTickWindows and FreeTickWindows, so the stepper and the rule are
// one piece of code.
//
// An edge goes to the first tick whose window end is after its stamp
// (windows are half-open: a stamp equal to a window end belongs to the next
// tick); edges stamped after the last window go to the last tick. In a
// frame with no tick, every edge waits (kEdgeWaits) for the next frame's
// first tick, whose window starts where this frame's would have.

#ifndef SE_CORE_EDGE_PLACEMENT_H_
#define SE_CORE_EDGE_PLACEMENT_H_

#include <cstdint>
#include <span>

namespace se {

// PlaceEdge's answer in a frame without ticks.
inline constexpr int kEdgeWaits = -1;

// The index (0-based) of the tick of the frame whose window holds
// |stamp_ns|, given the frame's ascending window ends; kEdgeWaits when the
// frame has no tick.
[[nodiscard]] int PlaceEdge(
    std::uint64_t stamp_ns,
    std::span<const std::uint64_t> window_ends) noexcept;

// PlaceEdge for each of |stamps| (in any order) into |out|, which must be
// at least as long as |stamps|.
void PlaceEdges(std::span<const std::uint64_t> stamps,
                std::span<const std::uint64_t> window_ends,
                std::span<int> out) noexcept;

// Whether the edge stamped |stamp_ns| belongs to the tick whose window ends
// at |window_end_ns|: before it, or anything when it is the frame's last
// tick. This is PlaceEdge as each tick's latch sees it.
[[nodiscard]] constexpr bool EdgeBelongsToTick(std::uint64_t stamp_ns,
                                               std::uint64_t window_end_ns,
                                               bool last_tick) noexcept {
  return last_tick || stamp_ns < window_end_ns;
}

// Locked cadence: |out|[i] = prev_start_ns + span * (i + 1) / out.size()
// with span = start_ns - prev_start_ns (0 when start_ns <= prev_start_ns).
void LockedTickWindows(std::uint64_t prev_start_ns, std::uint64_t start_ns,
                       std::span<std::uint64_t> out) noexcept;

// Free cadence: |out|[i] = start_ns - (remainder_ns + (n - 1 - i) * 1e9 /
// tick_rate) for n = out.size(), clamped at 0.
void FreeTickWindows(std::uint64_t start_ns, std::uint64_t remainder_ns,
                     std::uint32_t tick_rate,
                     std::span<std::uint64_t> out) noexcept;

}  // namespace se

#endif  // SE_CORE_EDGE_PLACEMENT_H_
