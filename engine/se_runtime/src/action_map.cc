// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/action_map.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_runtime/user_settings.h"
#include "se_sim/game_module.h"

namespace se {
namespace {

static_assert(kPadButtons == SDL_GAMEPAD_BUTTON_COUNT,
              "kPadButtons must match SDL_GAMEPAD_BUTTON_COUNT");
static_assert(kPadAxes == SDL_GAMEPAD_AXIS_COUNT,
              "kPadAxes must match SDL_GAMEPAD_AXIS_COUNT");
static_assert(kMouseButtons == SDL_BUTTON_X2, "mouse buttons are 1..X2");

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::unexpected<Error> Bad(std::string message) {
  return Fail(ErrorCode::kInvalidArgument, std::move(message));
}

// The legacy config names (core/util/LegacyInput.cpp's scancodeFromName).
std::uint32_t LegacyKeycode(std::string_view lower) {
  if (lower == "esc" || lower == "escape") return SDLK_ESCAPE;
  if (lower == "enter" || lower == "return") return SDLK_RETURN;
  if (lower == "space") return SDLK_SPACE;
  if (lower == "tab") return SDLK_TAB;
  if (lower == "backspace") return SDLK_BACKSPACE;
  if (lower == "up") return SDLK_UP;
  if (lower == "down") return SDLK_DOWN;
  if (lower == "left") return SDLK_LEFT;
  if (lower == "right") return SDLK_RIGHT;
  return SDLK_UNKNOWN;
}

// One pad name: a button (SDL's names, plus south/east/west/north), or a
// half axis ("+leftx", "-lefty").
struct PadInput {
  bool axis = false;
  std::uint8_t index = 0;
  bool positive = true;
};

std::expected<PadInput, Error> ParsePadName(std::string_view name) {
  const std::string lower = Lower(name);
  if (lower.starts_with('+') || lower.starts_with('-')) {
    const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(lower.c_str());
    if (axis == SDL_GAMEPAD_AXIS_INVALID) {
      return Bad(std::format("unknown gamepad axis '{}'", name));
    }
    return PadInput{true, static_cast<std::uint8_t>(axis), lower[0] == '+'};
  }
  SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
  if (lower == "south") {
    button = SDL_GAMEPAD_BUTTON_SOUTH;
  } else if (lower == "east") {
    button = SDL_GAMEPAD_BUTTON_EAST;
  } else if (lower == "west") {
    button = SDL_GAMEPAD_BUTTON_WEST;
  } else if (lower == "north") {
    button = SDL_GAMEPAD_BUTTON_NORTH;
  } else {
    button = SDL_GetGamepadButtonFromString(lower.c_str());
  }
  if (button == SDL_GAMEPAD_BUTTON_INVALID) {
    return Bad(std::format("unknown gamepad button '{}'", name));
  }
  return PadInput{false, static_cast<std::uint8_t>(button), true};
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  return text;
}

}  // namespace

std::uint8_t MouseButtonFromName(std::string_view name) noexcept {
  constexpr std::array<std::string_view, kMouseButtons> kNames = {
      "mouse-left", "mouse-middle", "mouse-right", "mouse-x1", "mouse-x2"};
  for (std::size_t i = 0; i < kNames.size(); ++i) {
    if (name == kNames[i]) return static_cast<std::uint8_t>(i + 1);
  }
  return 0;
}

std::uint32_t KeycodeFromName(std::string_view name) {
  if (name.empty() || MouseButtonFromName(name) != 0) return SDLK_UNKNOWN;
  const std::string lower = Lower(name);
  if (const std::uint32_t legacy = LegacyKeycode(lower);
      legacy != SDLK_UNKNOWN) {
    return legacy;
  }
  // One ASCII letter or digit is the keycode of that character (keycodes of
  // letters are lower case).
  if (lower.size() == 1 &&
      std::isalnum(static_cast<unsigned char>(lower[0])) != 0) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(lower[0]));
  }
  const std::string text(name);
  return SDL_GetKeyFromName(text.c_str());
}

std::expected<ActionMap, Error> ActionMap::Build(
    const ActionMapDefaults& defaults, const UserSettings& settings) {
  ActionMap map;
  if (defaults.bindings.size() > static_cast<std::size_t>(kMaxActions)) {
    return Bad(std::format("{} actions declared; at most {}",
                           defaults.bindings.size(), kMaxActions));
  }
  // Every user binding must name an action's config key.
  for (const auto& [config_key, key] : settings.key_bindings) {
    const bool declared = std::ranges::any_of(
        defaults.bindings, [&config_key](const ActionBinding& binding) {
          return !binding.config_key.empty() &&
                 binding.config_key == config_key;
        });
    if (!declared) {
      return Bad(
          std::format("key binding '{}' ('{}') names no action of the game",
                      config_key, key));
    }
  }
  for (std::size_t i = 0; i < defaults.bindings.size(); ++i) {
    const ActionBinding& declared = defaults.bindings[i];
    const std::uint64_t bit = 1ULL << i;
    Binding binding;
    binding.action = std::string(declared.action);
    binding.key = std::string(declared.key);
    binding.pad = std::string(declared.pad);
    if (!declared.config_key.empty()) {
      for (const auto& [config_key, key] : settings.key_bindings) {
        if (config_key == declared.config_key) {
          binding.key = key;
          binding.from_settings = true;
        }
      }
    }
    if (!binding.key.empty()) {
      binding.mouse_button = MouseButtonFromName(binding.key);
      if (binding.mouse_button != 0) {
        map.mouse_[binding.mouse_button] |= bit;
      } else {
        binding.keycode = KeycodeFromName(binding.key);
        if (binding.keycode == SDLK_UNKNOWN) {
          return Bad(std::format(
              "action '{}': unknown key '{}'{}", binding.action, binding.key,
              binding.from_settings
                  ? std::format(" (control-settings/key-bindings/{})",
                                declared.config_key)
                  : std::string()));
        }
        auto it =
            std::ranges::find(map.keys_, binding.keycode,
                              &std::pair<std::uint32_t, std::uint64_t>::first);
        if (it == map.keys_.end()) {
          map.keys_.emplace_back(binding.keycode, bit);
        } else {
          it->second |= bit;
        }
      }
    }
    std::string_view pads = declared.pad;
    while (!pads.empty()) {
      const std::size_t comma = pads.find(',');
      const std::string_view name = Trim(pads.substr(0, comma));
      pads = comma == std::string_view::npos ? std::string_view()
                                             : pads.substr(comma + 1);
      if (name.empty()) continue;
      auto pad = ParsePadName(name);
      if (!pad) {
        return Bad(std::format("action '{}': {}", binding.action,
                               pad.error().message));
      }
      if (pad->axis) {
        map.pad_axes_[pad->index][pad->positive ? 1 : 0] |= bit;
      } else {
        map.pad_buttons_[pad->index] |= bit;
      }
    }
    map.bindings_.push_back(std::move(binding));
  }
  std::ranges::sort(map.keys_);
  return map;
}

std::uint64_t ActionMap::ActionsForKey(std::uint32_t keycode) const noexcept {
  const auto it = std::ranges::lower_bound(
      keys_, keycode, {}, &std::pair<std::uint32_t, std::uint64_t>::first);
  return it != keys_.end() && it->first == keycode ? it->second : 0;
}

std::uint64_t ActionMap::ActionsForMouse(std::uint8_t button) const noexcept {
  return button < mouse_.size() ? mouse_[button] : 0;
}

std::uint64_t ActionMap::ActionsForPadButton(
    std::uint8_t button) const noexcept {
  return button < pad_buttons_.size() ? pad_buttons_[button] : 0;
}

std::uint64_t ActionMap::ActionsForPadAxis(std::uint8_t axis,
                                           bool positive) const noexcept {
  return axis < pad_axes_.size() ? pad_axes_[axis][positive ? 1 : 0] : 0;
}

bool ActionMap::AxisBound(std::uint8_t axis) const noexcept {
  return axis < pad_axes_.size() &&
         (pad_axes_[axis][0] != 0 || pad_axes_[axis][1] != 0);
}

}  // namespace se
