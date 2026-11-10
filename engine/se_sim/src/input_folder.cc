// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/input_folder.h"

#include <cstddef>
#include <cstdint>

#include "se_core/input_frame.h"

namespace se {

InputFrame InputFolder::Latch(Tick tick, std::uint64_t latch_ns,
                              InputEventRing& ring) noexcept {
  InputFrame frame;
  frame.tick = tick;
  frame.platform_live = true;
  for (InputEvent* front = ring.Front();
       front != nullptr && front->stamp_ns <= latch_ns; front = ring.Front()) {
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
          if (((held_ >> a) & 1U) != 0) {
            Edge(frame, static_cast<ActionId>(a), false, event);
          }
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
  std::uint8_t& count = down ? frame.pressed[index] : frame.released[index];
  if (count < 255) ++count;
  if (down) {
    held_ |= bit;
  } else {
    held_ &= ~bit;
  }
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
  axes_ = {};
}

}  // namespace se
