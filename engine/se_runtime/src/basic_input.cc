// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/basic_input.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "se_core/input_frame.h"
#include "se_core/types.h"
#include "se_sim/game_module.h"

namespace se {
namespace {

static_assert(BasicInputService::kScancodes == SDL_SCANCODE_COUNT,
              "kScancodes must match SDL_SCANCODE_COUNT");

SDL_Scancode ScancodeOf(std::string_view name) {
  // The legacy bindings say "Enter"; SDL calls the key "Return".
  const std::string text =
      name == "Enter" ? std::string("Return") : std::string(name);
  return SDL_GetScancodeFromName(text.c_str());
}

std::uint8_t Saturating(std::uint8_t value) noexcept {
  return value == UINT8_MAX ? value : static_cast<std::uint8_t>(value + 1);
}

}  // namespace

BasicInputService::BasicInputService(const ActionMapDefaults& actions) {
  int action = 0;
  for (const ActionBinding& binding : actions.bindings) {
    if (action >= kMaxActions) break;
    const SDL_Scancode scancode = ScancodeOf(binding.key);
    if (scancode == SDL_SCANCODE_UNKNOWN) {
      unknown_keys_.emplace_back(binding.key);
    } else {
      actions_by_key_[static_cast<std::size_t>(scancode)] |= 1ULL << action;
    }
    ++action;
  }
}

std::uint64_t BasicInputService::ActionsFor(
    std::uint32_t scancode) const noexcept {
  return scancode < kScancodes ? actions_by_key_[scancode] : 0;
}

void BasicInputService::OnKey(std::uint32_t scancode, bool down,
                              bool repeat) noexcept {
  if (repeat || scancode >= kScancodes) return;
  if (key_down_[scancode] == down) return;
  key_down_[scancode] = down;
  const std::uint64_t mask = actions_by_key_[scancode];
  for (int action = 0; action < kMaxActions; ++action) {
    if (((mask >> action) & 1U) != 0) SetAction(action, down);
  }
}

void BasicInputService::SetAction(int action, bool down) noexcept {
  const auto index = static_cast<std::size_t>(action);
  const std::uint64_t bit = 1ULL << action;
  if (down) {
    if (holders_[index] == 0) {
      held_ |= bit;
      pressed_[index] = Saturating(pressed_[index]);
    }
    holders_[index] = Saturating(holders_[index]);
  } else if (holders_[index] > 0) {
    --holders_[index];
    if (holders_[index] == 0) {
      held_ &= ~bit;
      released_[index] = Saturating(released_[index]);
    }
  }
}

void BasicInputService::ClearHeld() noexcept {
  for (std::size_t key = 0; key < key_down_.size(); ++key) {
    if (key_down_[key]) OnKey(static_cast<std::uint32_t>(key), false, false);
  }
}

InputFrame BasicInputService::Latch(Tick tick,
                                    std::uint64_t /*tick_window_end_ns*/,
                                    bool /*last_tick_of_frame*/) {
  InputFrame frame;
  frame.tick = tick;
  frame.held = held_;
  frame.pressed = pressed_;
  frame.released = released_;
  frame.platform_live = true;
  pressed_ = {};
  released_ = {};
  return frame;
}

}  // namespace se
