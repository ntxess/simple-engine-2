// Copyright 2026 The simple-engine-2 Authors.
//
// The real se::WakeMain() (declared in se_core/thread_role.h; headless test
// binaries link a null definition instead). WakeMain pushes one registered
// SDL user event, the only event engine code ever pushes, so a main thread
// blocked in SDL_WaitEvent (SDL_HINT_MAIN_CALLBACK_RATE=waitevent, the idle
// editor) runs its next iterate. Wakes coalesce through an atomic flag: at
// most one wake event is pending. Main clears the flag when it receives the
// event (ConsumeWakeEvent), before it drains what the wake announced, so a
// producer that publishes its data and then calls WakeMain is never missed.
//
// WakeMain pushes only while the scope is open (ADR-047): from the first
// iterate, which runs once SDL_AppInit has returned, to the start of
// shutdown step 4. SDL changes its event-watch list on main at both ends
// (SDL_InitMainCallbacks adds SDL's own watcher after SDL_AppInit returns;
// step 4 removes the engine's event filter) and SDL_PushEvent reads that
// list without its lock, a race TSan reports in SDL_DispatchEventWatchList.
// Outside the open window, and before the event type is registered or
// after the scope ends, WakeMain does nothing: main is not waiting then,
// and the first iterate looks at everything a producer published earlier.
// Any other code that pushes SDL events from a thread other than main
// keeps to the same window.

#ifndef SE_PLATFORM_WAKE_H_
#define SE_PLATFORM_WAKE_H_

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <expected>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se {

// Registers the wake event type with SDL for its lifetime. Main thread,
// with SDL's event subsystem initialised.
class WakeEventScope {
 public:
  [[nodiscard]] static std::expected<WakeEventScope, Error> Register()
      SE_REQUIRES(g_main);

  WakeEventScope(WakeEventScope&& other) noexcept;
  WakeEventScope& operator=(WakeEventScope&& other) noexcept;
  WakeEventScope(const WakeEventScope&) = delete;
  WakeEventScope& operator=(const WakeEventScope&) = delete;
  ~WakeEventScope();

  // From the first iterate (SDL_AppInit has returned): WakeMain pushes.
  void Open() noexcept SE_REQUIRES(g_main);
  // Before the event filter is removed: WakeMain stops pushing. Returns
  // once no push that saw the scope open is still inside SDL_PushEvent.
  // The destructor closes too.
  void Close() noexcept SE_REQUIRES(g_main);

 private:
  WakeEventScope() noexcept = default;
  void Release() noexcept;

  bool active_ = false;
  bool open_ = false;
};

// The registered wake event type; 0 while none is registered.
[[nodiscard]] std::uint32_t WakeEventType() noexcept;

// Main thread: true if |event| is the wake event. Clears the pending flag,
// so the next WakeMain pushes a new event.
bool ConsumeWakeEvent(const SDL_Event& event) noexcept;

// Whether a wake event is pending (pushed and not yet consumed).
[[nodiscard]] bool WakePending() noexcept;

// Wake events pushed since the process started (tests).
[[nodiscard]] std::uint64_t WakeEventsPushed() noexcept;

}  // namespace se

#endif  // SE_PLATFORM_WAKE_H_
