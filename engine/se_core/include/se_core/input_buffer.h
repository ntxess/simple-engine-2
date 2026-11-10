// Copyright 2026 The simple-engine-2 Authors.
//
// The input buffer window (docs/design/mt-design.md "Input And Latency":
// "a 6-12 tick buffer window absorbs sampling jitter"; docs/milestone-
// specs.md M8). A press latched on tick t stays consumable for |window|
// ticks: on ticks t to t + window - 1, until a rule consumes it. The window
// is set per action by the game (6 to 12 ticks, default 8). Pure data and
// functions: the buffer is part of the World's state (a game keeps it in a
// component), so it is a plain aggregate the component registry can
// reflect.
//
//   BufferPresses(buffer, context.input);              // each tick
//   if (ConsumeBuffered(buffer, kJump, tick)) Jump();  // a rule

#ifndef SE_CORE_INPUT_BUFFER_H_
#define SE_CORE_INPUT_BUFFER_H_

#include <array>
#include <cstdint>
#include <type_traits>

#include "se_core/input_frame.h"
#include "se_core/types.h"

namespace se {

inline constexpr std::uint8_t kMinBufferTicks = 6;
inline constexpr std::uint8_t kDefaultBufferTicks = 8;
inline constexpr std::uint8_t kMaxBufferTicks = 12;

struct InputBuffer {
  // The tick of each action's newest press not yet consumed; 0 for none
  // (ticks count from 1).
  std::array<Tick, kMaxActions> pressed_at{};
  // Each action's window in ticks; 0 means kDefaultBufferTicks.
  std::array<std::uint8_t, kMaxActions> window{};

  friend bool operator==(const InputBuffer&, const InputBuffer&) = default;
};

static_assert(std::is_trivially_copyable_v<InputBuffer>);

// Sets |action|'s window, clamped to [kMinBufferTicks, kMaxBufferTicks].
void SetBufferWindow(InputBuffer& buffer, ActionId action,
                     std::uint8_t ticks) noexcept;
// |action|'s window in ticks.
[[nodiscard]] std::uint8_t BufferWindow(const InputBuffer& buffer,
                                        ActionId action) noexcept;

// Records the presses of |frame| at frame.tick: an action pressed in the
// tick (pressed[a] > 0) is buffered from then on, replacing an older
// unconsumed press. Presses whose window has passed are dropped.
void BufferPresses(InputBuffer& buffer, const InputFrame& frame) noexcept;

// Whether |action| has a buffered press that |now| is still inside the
// window of (pressed_at <= now < pressed_at + window).
[[nodiscard]] bool IsBuffered(const InputBuffer& buffer, ActionId action,
                              Tick now) noexcept;

// IsBuffered, then removes the press: true at most once per press.
bool ConsumeBuffered(InputBuffer& buffer, ActionId action, Tick now) noexcept;

}  // namespace se

#endif  // SE_CORE_INPUT_BUFFER_H_
