// Copyright 2026 The simple-engine-2 Authors.
//
// Game code the App runs while it shuts down (GameSetup::on_shutdown, every
// GameState::OnExit) goes through RunGameHook, so that a throw from it is
// contained and the shutdown order still runs to its end: the log worker
// drains and the watchdog stops (ADR-044). The definition is in sdl_app.cc,
// the runtime's fault boundary; the App's teardown only ever runs inside an
// SDL callback (SDL_AppQuit, or SDL_AppInit undoing a failed boot).

#ifndef ENGINE_SE_RUNTIME_SRC_GAME_HOOK_H_
#define ENGINE_SE_RUNTIME_SRC_GAME_HOOK_H_

#include <string_view>

namespace se {

using GameHookBody = void (*)(void* context);

// Runs body(context). A throw is logged ("fault in <where>: <what>"), a
// dump is requested and false is returned; nothing is shown to the user,
// because the shutdown deadline is already running.
bool RunGameHook(std::string_view where, GameHookBody body,
                 void* context) noexcept;

// Runs body() the same way.
template <class F>
bool RunGameHook(std::string_view where, F& body) noexcept {
  return RunGameHook(
      where, [](void* context) { (*static_cast<F*>(context))(); }, &body);
}

}  // namespace se

#endif  // ENGINE_SE_RUNTIME_SRC_GAME_HOOK_H_
