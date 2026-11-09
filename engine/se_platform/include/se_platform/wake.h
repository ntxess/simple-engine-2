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
// Before WakeEventScope registers the event type, and after it ends,
// WakeMain does nothing.

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

 private:
  WakeEventScope() noexcept = default;
  void Release() noexcept;

  bool active_ = false;
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
