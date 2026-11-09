// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/inbox.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

#include <cstdint>
#include <expected>
#include <mutex>
#include <span>
#include <utility>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"

namespace se {

bool IsImmediateDispatchEvent(std::uint32_t type) noexcept {
  switch (type) {
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_LOW_MEMORY:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
      return true;
    default:
      return false;
  }
}

bool Inbox::Push(const SDL_Event& event) noexcept {
  {
    const std::scoped_lock lock(mutex_);
    if (pending_count_ < kCapacity) {
      buffers_[pending_index_][pending_count_++] = event;
      diverted_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  dropped_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

std::span<const SDL_Event> Inbox::TryTake() noexcept {
  SE_ASSERT_MAIN_THREAD();
  if (!mutex_.try_lock()) {
    busy_takes_.fetch_add(1, std::memory_order_relaxed);
    return {};
  }
  // Swap: the filled buffer becomes main's, main's becomes the pending one.
  const std::size_t taken_index = pending_index_;
  taken_count_ = pending_count_;
  pending_index_ = 1 - pending_index_;
  pending_count_ = 0;
  mutex_.unlock();
  return std::span<const SDL_Event>(buffers_[taken_index].data(), taken_count_);
}

bool SDLCALL EngineEventFilter(void* userdata, SDL_Event* event) noexcept {
  if (event == nullptr || !IsImmediateDispatchEvent(event->type) ||
      SDL_IsMainThread()) {
    return true;
  }
  // Off main: SDL's watchers would dispatch this here and drain the queue on
  // this thread. Keep a copy for main and drop the event.
  static_cast<Inbox*>(userdata)->Push(*event);
  return false;
}

std::expected<EventFilter, Error> EventFilter::Install(Inbox& inbox) {
  SE_ASSERT_MAIN_THREAD();
  SDL_EventFilter current = nullptr;
  void* current_userdata = nullptr;
  if (SDL_GetEventFilter(&current, &current_userdata)) {
    return Fail(ErrorCode::kInvalidArgument,
                "an SDL event filter is already installed");
  }
  SDL_SetEventFilter(&EngineEventFilter, &inbox);
  return EventFilter(&inbox);
}

EventFilter::EventFilter(EventFilter&& other) noexcept
    : inbox_(std::exchange(other.inbox_, nullptr)) {}

EventFilter& EventFilter::operator=(EventFilter&& other) noexcept {
  if (this != &other) {
    Remove();
    inbox_ = std::exchange(other.inbox_, nullptr);
  }
  return *this;
}

EventFilter::~EventFilter() { Remove(); }

void EventFilter::Remove() noexcept {
  if (inbox_ == nullptr) return;
  SE_ASSERT_MAIN_THREAD();
  SDL_SetEventFilter(nullptr, nullptr);
  inbox_ = nullptr;
}

}  // namespace se
