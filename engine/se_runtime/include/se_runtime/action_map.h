// Copyright 2026 The simple-engine-2 Authors.
//
// ActionMap: which physical inputs hold which of the game's actions
// (docs/milestone-specs.md M8, ADR-074). Built at boot from the
// GameModule's ActionMapDefaults (ActionId i is binding i) and the user's
// key bindings (UserSettings::key_bindings, config/config.json
// "control-settings" / "key-bindings"):
//
//   - an action's key is its user binding when its config_key names one,
//     else its default key, resolved to an SDL keycode (the key that types
//     that character in the current layout, SDL_GetKeyFromName) with the
//     legacy config names accepted: "esc"/"escape", "enter"/"return",
//     "space", "tab", "backspace", "up", "down", "left", "right";
//     "mouse-left", "mouse-right", "mouse-middle", "mouse-x1" and
//     "mouse-x2" are mouse buttons;
//   - its gamepad inputs are the binding's pad names (SDL gamepad button
//     names, and half axes "+leftx" / "-lefty" that press past a threshold);
//   - several actions may share an input ("enter" holds key-enter and
//     key-back in rampage).
//
// A key or pad name SDL does not know, and a user binding no action
// declares, is a boot Error naming the binding and the key. Main thread
// (SDL's key names), at boot; lookups are pure.

#ifndef SE_RUNTIME_ACTION_MAP_H_
#define SE_RUNTIME_ACTION_MAP_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_runtime/user_settings.h"
#include "se_sim/game_module.h"

namespace se {

// SDL_GAMEPAD_BUTTON_COUNT and SDL_GAMEPAD_AXIS_COUNT (checked against SDL
// in action_map.cc).
inline constexpr int kPadButtons = 26;
inline constexpr int kPadAxes = 6;
// SDL_BUTTON_LEFT .. SDL_BUTTON_X2.
inline constexpr int kMouseButtons = 5;

class ActionMap {
 public:
  // What an action is bound to, for logs and tests.
  struct Binding {
    std::string action;
    // The key's name as written (user binding or default), and its
    // keycode (0 for a mouse button).
    std::string key;
    std::uint32_t keycode = 0;
    // 1-5 for a mouse button, 0 otherwise.
    std::uint8_t mouse_button = 0;
    // True when the key came from the user's settings.
    bool from_settings = false;
    std::string pad;
  };

  ActionMap() = default;

  [[nodiscard]] static std::expected<ActionMap, Error> Build(
      const ActionMapDefaults& defaults, const UserSettings& settings);

  // The actions an input holds (bit i: ActionId i).
  [[nodiscard]] std::uint64_t ActionsForKey(
      std::uint32_t keycode) const noexcept;
  [[nodiscard]] std::uint64_t ActionsForMouse(
      std::uint8_t button) const noexcept;
  [[nodiscard]] std::uint64_t ActionsForPadButton(
      std::uint8_t button) const noexcept;
  // A half axis: |positive| for the "+" half.
  [[nodiscard]] std::uint64_t ActionsForPadAxis(std::uint8_t axis,
                                                bool positive) const noexcept;
  // Whether any action uses a half of |axis|.
  [[nodiscard]] bool AxisBound(std::uint8_t axis) const noexcept;

  [[nodiscard]] std::size_t ActionCount() const noexcept {
    return bindings_.size();
  }
  [[nodiscard]] const std::vector<Binding>& Bindings() const noexcept {
    return bindings_;
  }

 private:
  std::vector<Binding> bindings_;
  // Sorted by keycode.
  std::vector<std::pair<std::uint32_t, std::uint64_t>> keys_;
  std::array<std::uint64_t, kMouseButtons + 1> mouse_{};
  std::array<std::uint64_t, kPadButtons> pad_buttons_{};
  // [axis][0: negative half, 1: positive half].
  std::array<std::array<std::uint64_t, 2>, kPadAxes> pad_axes_{};
};

// The keycode of a key name (SDL's names and the legacy ones above); 0 when
// unknown or a mouse name.
[[nodiscard]] std::uint32_t KeycodeFromName(std::string_view name);
// 1-5 for "mouse-left" .. "mouse-x2", else 0.
[[nodiscard]] std::uint8_t MouseButtonFromName(std::string_view name) noexcept;

}  // namespace se

#endif  // SE_RUNTIME_ACTION_MAP_H_
