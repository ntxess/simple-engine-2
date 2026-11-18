// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/input_service.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "se_core/edge_placement.h"
#include "se_core/input_frame.h"
#include "se_core/types.h"
#include "se_runtime/action_map.h"

namespace se {
namespace {

std::uint8_t Saturating(std::uint8_t value) noexcept {
  return value == UINT8_MAX ? value : static_cast<std::uint8_t>(value + 1);
}

bool Earlier(std::uint64_t a_stamp, std::uint64_t a_seq, std::uint64_t b_stamp,
             std::uint64_t b_seq) noexcept {
  return a_stamp != b_stamp ? a_stamp < b_stamp : a_seq < b_seq;
}

float AxisValue(std::int16_t raw) noexcept {
  return std::clamp(static_cast<float>(raw) / 32767.0F, -1.0F, 1.0F);
}

}  // namespace

InputService::InputService(ActionMap map, InputServiceConfig config)
    : map_(std::move(map)), config_(config) {
  pending_.reserve(kMaxPending + 1);
  taken_.reserve(kMaxPending + 1);
  axis_pending_.reserve(kMaxPending + 1);
}

bool InputService::HandleEvent(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      OnKey(static_cast<std::uint32_t>(event.key.key), event.key.down,
            event.key.repeat, event.key.timestamp);
      return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      OnMouseButton(event.button.button, event.button.down,
                    event.button.timestamp);
      return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnPadButton(static_cast<std::uint32_t>(event.gbutton.which),
                  event.gbutton.button, event.gbutton.down,
                  event.gbutton.timestamp);
      return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnPadAxis(static_cast<std::uint32_t>(event.gaxis.which), event.gaxis.axis,
                AxisValue(event.gaxis.value), event.gaxis.timestamp);
      return true;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnPadRemoved(static_cast<std::uint32_t>(event.gdevice.which),
                   event.gdevice.timestamp);
      return true;
    default:
      return false;
  }
}

void InputService::OnKey(std::uint32_t keycode, bool down, bool repeat,
                         std::uint64_t stamp_ns) {
  ++stats_.events;
  if (repeat) return;
  const Physical input{Device::kKey, 0, keycode};
  if (down) {
    Press(input, map_.ActionsForKey(keycode), config_.key_source, stamp_ns);
  } else {
    Release(input, stamp_ns, config_.key_source);
  }
}

void InputService::OnMouseButton(std::uint8_t button, bool down,
                                 std::uint64_t stamp_ns) {
  ++stats_.events;
  const Physical input{Device::kMouse, 0, button};
  if (down) {
    Press(input, map_.ActionsForMouse(button), EdgeSource::kMouse, stamp_ns);
  } else {
    Release(input, stamp_ns, EdgeSource::kMouse);
  }
}

void InputService::OnPadButton(std::uint32_t pad, std::uint8_t button,
                               bool down, std::uint64_t stamp_ns) {
  ++stats_.events;
  const Physical input{Device::kPadButton, pad, button};
  if (down) {
    Press(input, map_.ActionsForPadButton(button), EdgeSource::kGamepadSampler,
          stamp_ns);
  } else {
    Release(input, stamp_ns, EdgeSource::kGamepadSampler);
  }
}

void InputService::OnPadAxis(std::uint32_t pad, std::uint8_t axis, float value,
                             std::uint64_t stamp_ns) {
  ++stats_.events;
  if (axis >= kMaxAxes) return;
  axis_pending_.push_back(AxisSample{stamp_ns, seq_++, axis, value});
  if (axis_pending_.size() > kMaxPending) {
    // Nothing latches: the oldest sample becomes the latched value.
    axes_[axis_pending_.front().axis] = axis_pending_.front().value;
    axis_pending_.erase(axis_pending_.begin());
  }
  if (!map_.AxisBound(axis)) return;
  for (const bool positive : {false, true}) {
    const Physical input{
        Device::kPadAxis, pad,
        static_cast<std::uint32_t>(axis) * 2U + (positive ? 1U : 0U)};
    const float deflection = positive ? value : -value;
    const bool is_down = std::ranges::any_of(
        down_, [&input](const Down& d) { return d.input == input; });
    if (!is_down && deflection >= config_.axis_press) {
      Press(input, map_.ActionsForPadAxis(axis, positive),
            EdgeSource::kGamepadSampler, stamp_ns);
    } else if (is_down && deflection < config_.axis_release) {
      Release(input, stamp_ns, EdgeSource::kGamepadSampler);
    }
  }
}

void InputService::OnPadRemoved(std::uint32_t pad, std::uint64_t stamp_ns) {
  std::vector<Physical> released;
  for (const Down& d : down_) {
    if ((d.input.device == Device::kPadButton ||
         d.input.device == Device::kPadAxis) &&
        d.input.pad == pad) {
      released.push_back(d.input);
    }
  }
  for (const Physical& input : released) {
    Release(input, stamp_ns, EdgeSource::kGamepadSampler);
  }
}

void InputService::ClearHeld(std::uint64_t stamp_ns) {
  ++stats_.clears;
  while (!down_.empty()) {
    const Physical input = down_.back().input;
    Release(input, stamp_ns, EdgeSource::kCleared);
  }
  for (std::size_t axis = 0; axis < axes_.size(); ++axis) {
    axis_pending_.push_back(
        AxisSample{stamp_ns, seq_++, static_cast<std::uint8_t>(axis), 0.0F});
  }
}

void InputService::Press(const Physical& input, std::uint64_t actions,
                         EdgeSource source, std::uint64_t stamp_ns) {
  for (const Down& d : down_) {
    if (d.input == input) return;  // already down
  }
  down_.push_back(Down{input, actions, source});
  if (actions == 0) return;
  Queue(Change{stamp_ns, seq_++, actions, true, source});
}

void InputService::Release(const Physical& input, std::uint64_t stamp_ns,
                           EdgeSource source) {
  const auto it = std::ranges::find_if(
      down_, [&input](const Down& d) { return d.input == input; });
  if (it == down_.end()) return;
  const std::uint64_t actions = it->actions;
  down_.erase(it);
  if (actions == 0) return;
  Queue(Change{stamp_ns, seq_++, actions, false, source});
}

void InputService::Queue(const Change& change) {
  pending_.push_back(change);
  if (pending_.size() > kMaxPending) FoldOldest();
}

void InputService::FoldOldest() {
  const auto oldest =
      std::ranges::min_element(pending_, [](const Change& a, const Change& b) {
        return Earlier(a.stamp_ns, a.seq, b.stamp_ns, b.seq);
      });
  Apply(*oldest, nullptr);
  ++stats_.changes_folded;
  pending_.erase(oldest);
}

void InputService::Apply(const Change& change, InputFrame* frame) noexcept {
  for (int a = 0; a < kMaxActions; ++a) {
    if (((change.actions >> a) & 1U) == 0) continue;
    const auto index = static_cast<std::size_t>(a);
    const std::uint64_t bit = 1ULL << a;
    bool edge = false;
    if (change.down) {
      edge = holders_[index] == 0;
      if (holders_[index] < UINT16_MAX) ++holders_[index];
      if (edge) held_ |= bit;
    } else if (holders_[index] > 0) {
      --holders_[index];
      edge = holders_[index] == 0;
      if (edge) held_ &= ~bit;
    }
    if (!edge || frame == nullptr) continue;
    ++stats_.edges;
    if (change.down) {
      frame->pressed[index] = Saturating(frame->pressed[index]);
    } else {
      frame->released[index] = Saturating(frame->released[index]);
    }
    if (frame->edge_count >= kMaxEdgesPerTick) {
      ++stats_.edges_not_stored;
      continue;
    }
    std::uint8_t order = 0;
    if (frame->edge_count > 0) {
      const InputEdge& previous = frame->edges[frame->edge_count - 1];
      if (previous.stamp_ns == change.stamp_ns) {
        order = Saturating(previous.order);
      }
    }
    frame->edges[frame->edge_count++] =
        InputEdge{static_cast<ActionId>(a), change.down, change.stamp_ns, order,
                  change.source};
  }
}

InputFrame InputService::Latch(Tick tick, std::uint64_t tick_window_end_ns,
                               bool last_tick_of_frame) {
  InputFrame frame;
  frame.tick = tick;
  frame.platform_live = true;

  // The changes of this tick, in (stamp, arrival) order.
  taken_.clear();
  std::size_t kept = 0;
  for (const Change& change : pending_) {
    if (EdgeBelongsToTick(change.stamp_ns, tick_window_end_ns,
                          last_tick_of_frame)) {
      taken_.push_back(change);
    } else {
      // |kept| never passes the element being read.
      pending_[kept++] = change;
    }
  }
  pending_.resize(kept);
  std::ranges::sort(taken_, [](const Change& a, const Change& b) {
    return Earlier(a.stamp_ns, a.seq, b.stamp_ns, b.seq);
  });
  for (const Change& change : taken_) {
    if (change.stamp_ns < last_window_end_ns_) ++stats_.late_changes;
    Apply(change, &frame);
  }

  // Axis samples the same way: the newest of each axis within the window.
  std::ranges::sort(axis_pending_,
                    [](const AxisSample& a, const AxisSample& b) {
                      return Earlier(a.stamp_ns, a.seq, b.stamp_ns, b.seq);
                    });
  std::size_t axis_kept = 0;
  for (const AxisSample& sample : axis_pending_) {
    if (EdgeBelongsToTick(sample.stamp_ns, tick_window_end_ns,
                          last_tick_of_frame)) {
      axes_[sample.axis] = sample.value;
    } else {
      axis_pending_[axis_kept++] = sample;
    }
  }
  axis_pending_.resize(axis_kept);

  frame.held = held_;
  frame.axes = axes_;
  last_window_end_ns_ = std::max(last_window_end_ns_, tick_window_end_ns);
  return frame;
}

}  // namespace se
