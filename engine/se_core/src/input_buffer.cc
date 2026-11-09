// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/input_buffer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "se_core/input_frame.h"
#include "se_core/types.h"

namespace se {

void SetBufferWindow(InputBuffer& buffer, ActionId action,
                     std::uint8_t ticks) noexcept {
  if (action >= kMaxActions) return;
  buffer.window[action] = std::clamp(ticks, kMinBufferTicks, kMaxBufferTicks);
}

std::uint8_t BufferWindow(const InputBuffer& buffer, ActionId action) noexcept {
  if (action >= kMaxActions) return kDefaultBufferTicks;
  const std::uint8_t window = buffer.window[action];
  return window == 0 ? kDefaultBufferTicks : window;
}

bool IsBuffered(const InputBuffer& buffer, ActionId action, Tick now) noexcept {
  if (action >= kMaxActions) return false;
  const Tick at = buffer.pressed_at[action];
  return at != 0 && at <= now && now - at < BufferWindow(buffer, action);
}

void BufferPresses(InputBuffer& buffer, const InputFrame& frame) noexcept {
  for (std::size_t a = 0; a < static_cast<std::size_t>(kMaxActions); ++a) {
    const auto action = static_cast<ActionId>(a);
    if (frame.pressed[a] > 0) {
      buffer.pressed_at[a] = frame.tick;
    } else if (buffer.pressed_at[a] != 0 &&
               !IsBuffered(buffer, action, frame.tick)) {
      buffer.pressed_at[a] = 0;
    }
  }
}

bool ConsumeBuffered(InputBuffer& buffer, ActionId action, Tick now) noexcept {
  if (!IsBuffered(buffer, action, now)) return false;
  buffer.pressed_at[action] = 0;
  return true;
}

}  // namespace se
