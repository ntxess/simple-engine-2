// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/input_folder.h"

#include <cstddef>
#include <cstdint>

#include "se_core/edge_placement.h"
#include "se_core/input_frame.h"

namespace se {

InputFrame InputFolder::Latch(Tick tick, std::uint64_t latch_ns,
                              InputEventRing& ring) noexcept {
  InputFrame frame;
  frame.tick = tick;
  frame.platform_live = true;
  // M8's rule with the deadline as the window end: half-open, and no tick
  // takes the events after its window (there is no frame's last tick).
  for (InputEvent* front = ring.Front();
       front != nullptr &&
       EdgeBelongsToTick(front->stamp_ns, latch_ns, /*last_tick=*/false);
       front = ring.Front()) {
    const InputEvent event = *front;
    ring.Pop();
    ++stats_.events;
    if (event.stamp_ns > stats_.last_stamp_ns) {
      stats_.last_stamp_ns = event.stamp_ns;
    }
    switch (event.kind) {
      case InputKind::kPress:
      case InputKind::kRelease:
        if (event.action >= kMaxActions) {
          ++stats_.ignored;
          break;
        }
        Edge(frame, event.action, event.kind == InputKind::kPress, event);
        break;
      case InputKind::kAxis:
        if (event.action >= kMaxAxes) {
          ++stats_.ignored;
          break;
        }
        axes_[event.action] = event.value;
        break;
      case InputKind::kReleaseAll:
        for (int a = 0; a < kMaxActions; ++a) {
          const auto index = static_cast<std::size_t>(a);
          if (holders_[index] == 0) continue;
          // The last holder's release makes the edge.
          holders_[index] = 1;
          Edge(frame, static_cast<ActionId>(a), false, event);
        }
        break;
      case InputKind::kHeartbeat:
        ++stats_.heartbeats;
        break;
    }
  }
  frame.held = held_;
  frame.axes = axes_;
  return frame;
}

void InputFolder::Edge(InputFrame& frame, ActionId action, bool down,
                       const InputEvent& event) noexcept {
  const std::uint64_t bit = std::uint64_t{1} << action;
  const auto index = static_cast<std::size_t>(action);
  std::uint16_t& holders = holders_[index];
  if (down) {
    const bool first = holders == 0;
    if (holders < UINT16_MAX) ++holders;
    if (!first) return;
    held_ |= bit;
  } else {
    if (holders == 0) {
      ++stats_.stray_releases;
      return;
    }
    if (--holders != 0) return;
    held_ &= ~bit;
  }
  std::uint8_t& count = down ? frame.pressed[index] : frame.released[index];
  if (count < 255) ++count;
  if (frame.edge_count >= kMaxEdgesPerTick) {
    ++stats_.edges_dropped;
    return;
  }
  // Order among this frame's edges with the same stamp.
  std::uint8_t order = 0;
  for (std::uint8_t i = 0; i < frame.edge_count; ++i) {
    if (frame.edges[i].stamp_ns == event.stamp_ns) ++order;
  }
  frame.edges[frame.edge_count++] =
      InputEdge{action, down, event.stamp_ns, order, event.source};
}

void InputFolder::Reset() noexcept {
  held_ = 0;
  holders_ = {};
  axes_ = {};
}

}  // namespace se
