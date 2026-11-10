// Copyright 2026 The simple-engine-2 Authors.
//
// The open gamepads (docs/milestone-specs.md M8). SDL delivers gamepad
// button and axis events only for gamepads that are open, so main opens
// each one when SDL_EVENT_GAMEPAD_ADDED arrives and closes it on
// SDL_EVENT_GAMEPAD_REMOVED; the destructor closes the rest. The input
// sampler (input_sampler.h) updates them; with
// SDL_HINT_AUTO_UPDATE_JOYSTICKS=0 nothing else does. Main thread only.

#ifndef SE_PLATFORM_GAMEPADS_H_
#define SE_PLATFORM_GAMEPADS_H_

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

#include <cstddef>
#include <string>
#include <vector>

namespace se {

class Gamepads {
 public:
  Gamepads() = default;
  Gamepads(const Gamepads&) = delete;
  Gamepads& operator=(const Gamepads&) = delete;
  ~Gamepads();

  // SDL_EVENT_GAMEPAD_ADDED and SDL_EVENT_GAMEPAD_REMOVED; other events are
  // ignored. Returns true when it opened or closed one.
  bool HandleEvent(const SDL_Event& event);
  void CloseAll();

  [[nodiscard]] std::size_t Count() const noexcept { return pads_.size(); }
  // The name of the most recently opened gamepad ("" when none).
  [[nodiscard]] const std::string& LastName() const noexcept {
    return last_name_;
  }

 private:
  struct Pad {
    SDL_JoystickID id = 0;
    SDL_Gamepad* gamepad = nullptr;
  };
  std::vector<Pad> pads_;
  std::string last_name_;
};

}  // namespace se

#endif  // SE_PLATFORM_GAMEPADS_H_
