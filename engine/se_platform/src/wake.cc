// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/wake.h"

#include <SDL3/SDL_events.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <utility>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"

namespace se {
namespace {

std::atomic<std::uint32_t> g_wake_type{0};
std::atomic<bool> g_wake_pending{false};
std::atomic<std::uint64_t> g_wakes_pushed{0};

}  // namespace

// The definition se_core declares. Any thread. The flag uses sequentially
// consistent operations on both sides: a producer that finds a wake pending
// published its data before main clears the flag, and main drains after
// clearing it.
void WakeMain() noexcept {
  const std::uint32_t type = g_wake_type.load(std::memory_order_acquire);
  if (type == 0) return;
  if (g_wake_pending.exchange(true)) return;
  SDL_Event event{};
  event.type = type;
  if (SDL_PushEvent(&event)) {
    g_wakes_pushed.fetch_add(1, std::memory_order_relaxed);
  } else {
    // Not queued (SDL is shutting down, or the queue is full): let the next
    // call try again.
    g_wake_pending.store(false);
  }
}

std::expected<WakeEventScope, Error> WakeEventScope::Register() {
  SE_ASSERT_MAIN_THREAD();
  const std::uint32_t type = SDL_RegisterEvents(1);
  if (type == 0) {
    return Fail(ErrorCode::kUnsupported, "SDL_RegisterEvents failed");
  }
  g_wake_pending.store(false);
  g_wake_type.store(type, std::memory_order_release);
  WakeEventScope scope;
  scope.active_ = true;
  return scope;
}

WakeEventScope::WakeEventScope(WakeEventScope&& other) noexcept
    : active_(std::exchange(other.active_, false)) {}

WakeEventScope& WakeEventScope::operator=(WakeEventScope&& other) noexcept {
  if (this != &other) {
    Release();
    active_ = std::exchange(other.active_, false);
  }
  return *this;
}

WakeEventScope::~WakeEventScope() { Release(); }

void WakeEventScope::Release() noexcept {
  if (!active_) return;
  g_wake_type.store(0, std::memory_order_release);
  g_wake_pending.store(false);
  active_ = false;
}

std::uint32_t WakeEventType() noexcept {
  return g_wake_type.load(std::memory_order_acquire);
}

bool ConsumeWakeEvent(const SDL_Event& event) noexcept {
  const std::uint32_t type = g_wake_type.load(std::memory_order_acquire);
  if (type == 0 || event.type != type) return false;
  g_wake_pending.store(false);
  return true;
}

bool WakePending() noexcept { return g_wake_pending.load(); }

std::uint64_t WakeEventsPushed() noexcept {
  return g_wakes_pushed.load(std::memory_order_relaxed);
}

}  // namespace se
