// Copyright 2026 The simple-engine-2 Authors.
//
// InputBuffer (se_core/input_buffer.h): a press latched on tick t is
// consumable on ticks t to t + window - 1, once; windows are 6 to 12 ticks,
// 8 by default, per action.

#include "se_core/input_buffer.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "se_core/input_frame.h"

namespace se {
namespace {

InputFrame Frame(Tick tick, ActionId pressed = 255) {
  InputFrame frame;
  frame.tick = tick;
  if (pressed < kMaxActions) frame.pressed[pressed] = 1;
  return frame;
}

TEST_CASE("A press stays consumable for its window", "[input_buffer]") {
  InputBuffer buffer;
  REQUIRE(BufferWindow(buffer, 4) == 8);
  BufferPresses(buffer, Frame(10, 4));
  for (Tick t = 10; t < 18; ++t) {
    REQUIRE(IsBuffered(buffer, 4, t));
  }
  CHECK_FALSE(IsBuffered(buffer, 4, 18));
  CHECK_FALSE(IsBuffered(buffer, 4, 9));
  CHECK_FALSE(IsBuffered(buffer, 5, 10));

  // Consumed once only.
  REQUIRE(ConsumeBuffered(buffer, 4, 15));
  CHECK_FALSE(ConsumeBuffered(buffer, 4, 15));
  CHECK_FALSE(IsBuffered(buffer, 4, 16));
}

TEST_CASE("Windows are per action and clamped to 6-12 ticks",
          "[input_buffer]") {
  InputBuffer buffer;
  SetBufferWindow(buffer, 1, 12);
  SetBufferWindow(buffer, 2, 3);
  SetBufferWindow(buffer, 3, 40);
  REQUIRE(BufferWindow(buffer, 1) == 12);
  REQUIRE(BufferWindow(buffer, 2) == kMinBufferTicks);
  REQUIRE(BufferWindow(buffer, 3) == kMaxBufferTicks);
  REQUIRE(BufferWindow(buffer, 0) == kDefaultBufferTicks);

  BufferPresses(buffer, Frame(100, 1));
  BufferPresses(buffer, Frame(100, 2));
  REQUIRE(IsBuffered(buffer, 1, 111));
  CHECK_FALSE(IsBuffered(buffer, 1, 112));
  REQUIRE(IsBuffered(buffer, 2, 105));
  CHECK_FALSE(IsBuffered(buffer, 2, 106));
}

TEST_CASE("A new press replaces an older one; expired presses go",
          "[input_buffer]") {
  InputBuffer buffer;
  BufferPresses(buffer, Frame(20, 7));
  BufferPresses(buffer, Frame(24, 7));
  REQUIRE(buffer.pressed_at[7] == 24);
  REQUIRE(IsBuffered(buffer, 7, 31));
  for (Tick t = 25; t <= 40; ++t) BufferPresses(buffer, Frame(t));
  REQUIRE(buffer.pressed_at[7] == 0);
  CHECK_FALSE(IsBuffered(buffer, 7, 40));
  // Out-of-range actions are ignored.
  CHECK_FALSE(IsBuffered(buffer, kMaxActions, 40));
  CHECK_FALSE(ConsumeBuffered(buffer, kMaxActions, 40));
}

}  // namespace
}  // namespace se
