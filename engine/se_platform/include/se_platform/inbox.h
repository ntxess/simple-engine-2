// Copyright 2026 The simple-engine-2 Authors.
//
// The event filter and the Inbox (docs/design/mt-design.md "Data Flow":
// TERMINATING, LOW_MEMORY and the four background/foreground events).
//
// With SDL's main callbacks, SDL's own event watcher dispatches six
// "immediate" event types on the thread that pushes them, and drains the
// whole event queue there first. The engine's filter runs on the pushing
// thread before any watcher (SDL_SetEventFilter). Off the main thread it
// copies an immediate event into the Inbox and returns false, so SDL drops
// it: no watcher runs and SDL never drains its queue on that thread. Every
// other event, and every event pushed on main, passes through unchanged.
//
// The main thread takes the diverted events at the start of each iterate
// with try_lock (a busy lock leaves them for the next iterate) and
// dispatches them with no lock held. It never calls SDL under the inbox
// lock. Lock order: SDL's event-watch lock (held while the filter runs),
// then the inbox lock; never the reverse.
//
// The Inbox holds at most kCapacity events between two takes in a fixed
// buffer (the "vector" of the design, swapped with main's at each take) and
// never allocates; a push into a full inbox is counted as dropped.

#ifndef SE_PLATFORM_INBOX_H_
#define SE_PLATFORM_INBOX_H_

#include <SDL3/SDL_events.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se {

// The six event types SDL's main callbacks dispatch on the pushing thread.
[[nodiscard]] bool IsImmediateDispatchEvent(std::uint32_t type) noexcept;

class Inbox {
 public:
  static constexpr std::size_t kCapacity = 64;

  Inbox() = default;
  Inbox(const Inbox&) = delete;
  Inbox& operator=(const Inbox&) = delete;

  // Copies |event| in. Any thread; false (and counted) when full.
  bool Push(const SDL_Event& event) noexcept;

  // Main thread: the events pushed since the last take, oldest first, or
  // none when another thread holds the lock (they stay for the next take).
  // The span is valid until the next call.
  [[nodiscard]] std::span<const SDL_Event> TryTake() noexcept
      SE_REQUIRES(g_main);

  // Events accepted into the inbox, and pushes refused because it was full.
  [[nodiscard]] std::uint64_t Diverted() const noexcept {
    return diverted_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t Dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }
  // Takes that found the lock busy.
  [[nodiscard]] std::uint64_t BusyTakes() const noexcept {
    return busy_takes_.load(std::memory_order_relaxed);
  }

 private:
  using Buffer = std::array<SDL_Event, kCapacity>;

  std::mutex mutex_;
  // Two buffers swapped at each take. Guarded by mutex_: which buffer is
  // pending and its count. The other buffer is main's.
  std::array<Buffer, 2> buffers_{};
  std::size_t pending_index_ = 0;
  std::size_t pending_count_ = 0;
  std::size_t taken_count_ = 0;
  std::atomic<std::uint64_t> diverted_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> busy_takes_{0};
};

// Installs the engine's event filter for |inbox| (SDL_SetEventFilter).
// Remove() or the destructor removes it. Only one filter can be installed;
// installing over another filter is an error.
class EventFilter {
 public:
  [[nodiscard]] static std::expected<EventFilter, Error> Install(Inbox& inbox)
      SE_REQUIRES(g_main);

  EventFilter(EventFilter&& other) noexcept;
  EventFilter& operator=(EventFilter&& other) noexcept;
  EventFilter(const EventFilter&) = delete;
  EventFilter& operator=(const EventFilter&) = delete;
  ~EventFilter();

  // Removes the filter (shutdown step 4). Main thread.
  void Remove() noexcept;
  [[nodiscard]] bool Installed() const noexcept { return inbox_ != nullptr; }

 private:
  explicit EventFilter(Inbox* inbox) noexcept : inbox_(inbox) {}

  Inbox* inbox_ = nullptr;
};

// The filter itself, exposed for tests: |userdata| is the Inbox.
bool SDLCALL EngineEventFilter(void* userdata, SDL_Event* event) noexcept;

}  // namespace se

#endif  // SE_PLATFORM_INBOX_H_
