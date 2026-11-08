// Copyright 2026 The simple-engine-2 Authors.

#include "support/stamped_input.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "se_core/guid.h"
#include "se_core/input_frame.h"
#include "se_core/rng.h"
#include "support/kernel_test_module.h"

namespace se::testing {
namespace {

constexpr std::uint32_t kEdgeStream = 0x57A0;
// Edges are at least kMinGap ticks apart, so their stamps stay in order.
constexpr Tick kMinGap = 11;
constexpr std::uint64_t kSpread = 9;
constexpr std::uint64_t kActions = kActionFire + 1;

}  // namespace

StampedInput::StampedInput(std::uint64_t seed, std::uint64_t epoch_ns,
                           std::uint32_t tick_rate) noexcept
    : seed_(seed),
      epoch_ns_(epoch_ns),
      dt_ns_(1e9 / static_cast<double>(tick_rate == 0 ? kDefaultTickRate
                                                      : tick_rate)),
      next_(Script()) {}

StampedInput::Edge StampedInput::Script() noexcept {
  const auto draw = [this](std::uint32_t n) {
    return rng::At(seed_, index_, Guid{}, kEdgeStream, n);
  };
  Edge edge;
  edge.tick = 1 + index_ * kMinGap + draw(0) % kSpread;
  edge.action = static_cast<ActionId>(draw(1) % kActions);
  const std::uint64_t bit = 1ULL << edge.action;
  edge.down = (scripted_held_ & bit) == 0;
  scripted_held_ ^= bit;
  const double offset = static_cast<double>(rng::Unit(draw(2))) * 0.5 - 0.25;
  edge.stamp_ns =
      epoch_ns_ +
      static_cast<std::uint64_t>(std::llround(
          (static_cast<double>(edge.tick) - 0.5 + offset) * dt_ns_));
  ++index_;
  return edge;
}

InputFrame StampedInput::Latch(Tick tick, std::uint64_t tick_window_end_ns,
                               bool /*last_tick_of_frame*/) {
  InputFrame frame;
  frame.tick = tick;
  while (next_.stamp_ns <= tick_window_end_ns) {
    const std::uint64_t bit = 1ULL << next_.action;
    const auto action = static_cast<std::size_t>(next_.action);
    if (next_.down) {
      held_ |= bit;
      ++frame.pressed[action];
    } else {
      held_ &= ~bit;
      ++frame.released[action];
    }
    if (frame.edge_count < kMaxEdgesPerTick) {
      frame.edges[frame.edge_count++] =
          InputEdge{next_.action, next_.down, next_.stamp_ns, 0,
                    EdgeSource::kRawKeyboard};
    }
    ++delivered_;
    next_ = Script();
  }
  frame.held = held_;
  return frame;
}

}  // namespace se::testing
