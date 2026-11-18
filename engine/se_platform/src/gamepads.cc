// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/gamepads.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <string>

#include "se_core/log.h"
#include "se_core/thread_role.h"

namespace se {

Gamepads::~Gamepads() { CloseAll(); }

bool Gamepads::HandleEvent(const SDL_Event& event) {
  if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
    SE_ASSERT_MAIN_THREAD();
    const SDL_JoystickID id = event.gdevice.which;
    const auto same = [id](const Pad& p) { return p.id == id; };
    if (std::ranges::any_of(pads_, same)) return false;
    SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
    if (gamepad == nullptr) {
      SE_LOG_WARN("gamepad {} could not be opened: {}", id, SDL_GetError());
      return false;
    }
    const char* name = SDL_GetGamepadName(gamepad);
    last_name_ = name != nullptr ? name : "";
    pads_.push_back(Pad{id, gamepad});
    SE_LOG_INFO("gamepad {} opened: '{}' ({} open)", id, last_name_,
                pads_.size());
    return true;
  }
  if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
    SE_ASSERT_MAIN_THREAD();
    const SDL_JoystickID id = event.gdevice.which;
    const auto it =
        std::ranges::find_if(pads_, [id](const Pad& p) { return p.id == id; });
    if (it == pads_.end()) return false;
    SDL_CloseGamepad(it->gamepad);
    pads_.erase(it);
    SE_LOG_INFO("gamepad {} closed ({} open)", id, pads_.size());
    return true;
  }
  return false;
}

void Gamepads::CloseAll() {
  if (pads_.empty()) return;
  SE_ASSERT_MAIN_THREAD();
  for (const Pad& pad : pads_) SDL_CloseGamepad(pad.gamepad);
  pads_.clear();
}

}  // namespace se
