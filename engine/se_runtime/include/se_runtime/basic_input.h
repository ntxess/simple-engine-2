// Copyright 2026 The simple-engine-2 Authors.
//
// BasicInputService: the minimal InputSource of M5 (docs/milestone-specs.md
// M5, "The frame"). It maps keys to the GameModule's actions with the
// module's default bindings (ActionMapDefaults: binding i is ActionId i,
// its key an SDL scancode name), keeps the held state from key events on
// main, and latches one InputFrame per tick: the held bits, and the presses
// and releases since the previous latch (they all land on the next tick).
// M8 replaces it with the InputService that places timestamped edges.
//
// Key names are SDL's scancode names; "Enter" is accepted for "Return".
// Several actions may share a key.

#ifndef SE_RUNTIME_BASIC_INPUT_H_
#define SE_RUNTIME_BASIC_INPUT_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "se_core/input_frame.h"
#include "se_core/types.h"
#include "se_sim/game_module.h"

namespace se {

class BasicInputService final : public InputSource {
 public:
  // SDL_SCANCODE_COUNT; checked against SDL in basic_input.cc.
  static constexpr int kScancodes = 512;

  explicit BasicInputService(const ActionMapDefaults& actions);

  // A key event on main: |scancode| is SDL's; repeats are ignored.
  void OnKey(std::uint32_t scancode, bool down, bool repeat) noexcept;
  // Releases every held action (focus lost, modal loop): counted as
  // releases on the next latch.
  void ClearHeld() noexcept;

  InputFrame Latch(Tick tick, std::uint64_t tick_window_end_ns,
                   bool last_tick_of_frame) override;

  [[nodiscard]] std::uint64_t Held() const noexcept { return held_; }
  // Bindings whose key name SDL did not recognise.
  [[nodiscard]] const std::vector<std::string>& UnknownKeys() const noexcept {
    return unknown_keys_;
  }
  // Actions bound to |scancode| (bit i: action i).
  [[nodiscard]] std::uint64_t ActionsFor(std::uint32_t scancode) const noexcept;

 private:
  void SetAction(int action, bool down) noexcept;

  std::array<std::uint64_t, kScancodes> actions_by_key_{};
  std::array<bool, kScancodes> key_down_{};
  // How many held keys hold each action.
  std::array<std::uint8_t, kMaxActions> holders_{};
  std::uint64_t held_ = 0;
  std::array<std::uint8_t, kMaxActions> pressed_{};
  std::array<std::uint8_t, kMaxActions> released_{};
  std::vector<std::string> unknown_keys_;
};

}  // namespace se

#endif  // SE_RUNTIME_BASIC_INPUT_H_
