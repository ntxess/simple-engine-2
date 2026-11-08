// Copyright 2026 The simple-engine-2 Authors.
//
// StampedInput: input as edges stamped on the wall clock, latched into
// ticks by the stepper's tick windows the way the platform's InputService
// does (docs/design/mt-design.md, input): each edge goes to the first tick
// whose window ends at or after its stamp.
//
// The edges are a seeded script placed on SyntheticFrameClock's tick grid:
// edge e belongs to tick t_e and is stamped at
//   epoch + (t_e - 1/2 + o_e) * dt,  o_e uniform in [-1/4, 1/4),
// so it lies inside tick t_e's window whenever every window end is within a
// quarter tick of epoch + k * dt. A regular clock (jitter under dt / 4)
// keeps them there at any rate, which makes the latched InputFrames
// independent of the frame clock; a clock that misses vblanks compresses a
// frame's windows, so edges may land a tick early or late (a replay records
// where they landed).
//
// A scripted source has no "now", so it does not hand later edges to a
// frame's last tick (last_tick_of_frame); they wait for the tick whose
// window holds them.

#ifndef TESTS_SUPPORT_STAMPED_INPUT_H_
#define TESTS_SUPPORT_STAMPED_INPUT_H_

#include <cstdint>

#include "se_core/input_frame.h"
#include "se_core/sim_clock.h"
#include "se_core/types.h"

namespace se::testing {

class StampedInput final : public InputSource {
 public:
  StampedInput(std::uint64_t seed, std::uint64_t epoch_ns,
               std::uint32_t tick_rate = kDefaultTickRate) noexcept;

  InputFrame Latch(Tick tick, std::uint64_t tick_window_end_ns,
                   bool last_tick_of_frame) override;

  // Edges delivered so far.
  [[nodiscard]] std::uint64_t Delivered() const noexcept { return delivered_; }

 private:
  struct Edge {
    Tick tick = 0;
    std::uint64_t stamp_ns = 0;
    ActionId action = 0;
    bool down = false;
  };

  // Scripts edge |index_| (after the previous one).
  [[nodiscard]] Edge Script() noexcept;

  std::uint64_t seed_;
  std::uint64_t epoch_ns_;
  double dt_ns_;
  std::uint64_t index_ = 0;
  // Held bits as scripted, and as latched.
  std::uint64_t scripted_held_ = 0;
  std::uint64_t held_ = 0;
  Edge next_;
  std::uint64_t delivered_ = 0;
};

}  // namespace se::testing

#endif  // TESTS_SUPPORT_STAMPED_INPUT_H_
