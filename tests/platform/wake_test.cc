// Copyright 2026 The simple-engine-2 Authors.
//
// WakeMain: wakes a main thread blocked in SDL_WaitEvent (the waitevent
// callback rate of the idle editor), and coalesces to one pending event.
// Run under TSan in WSL.

#include "se_platform/wake.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "platform/support/callback_loop.h"
#include "se_core/thread_role.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::CallbackLoop;
using testing::NowSec;
using testing::RunCallbackLoop;

int CountQueued(std::uint32_t type) {
  return SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, type, type);
}

TEST_CASE("WakeMain does nothing before the wake event is registered",
          "[wake]") {
  const testing::MainThreadScope main;
  REQUIRE(WakeEventType() == 0);
  const std::uint64_t before = WakeEventsPushed();
  WakeMain();
  CHECK(WakeEventsPushed() == before);
  CHECK_FALSE(WakePending());
}

TEST_CASE("WakeMain wakes a main thread blocked in waitevent", "[wake]") {
  const testing::MainThreadScope main;
  std::optional<WakeEventScope> wake;
  std::thread waker;
  std::atomic<double> wake_called_at{0.0};
  std::atomic<bool> stop_waker{false};
  double blocked_at = 0.0;
  double woken_iterate_at = 0.0;
  int iterations = 0;
  bool woke = false;
  std::uint64_t pushed_before = 0;

  CallbackLoop loop;
  loop.init = [&](void** /*appstate*/, int /*argc*/, char** /*argv*/) {
    AssertMainThread();
    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) return SDL_APP_FAILURE;
    auto registered = WakeEventScope::Register();
    if (!registered) return SDL_APP_FAILURE;
    wake.emplace(std::move(*registered));
    return SDL_APP_CONTINUE;
  };
  loop.iterate = [&](void* /*appstate*/) {
    ++iterations;
    if (iterations == 1) {
      // The first iterate runs once SDL has installed its own watcher (the
      // waker must not push while SDL adds it). From here on, iterate only
      // after an event.
      SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "waitevent");
      // Drop anything SDL queued while initialising, so main really blocks.
      SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
      pushed_before = WakeEventsPushed();
      blocked_at = NowSec();
      waker = std::thread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        wake_called_at.store(NowSec());
        WakeMain();
        // Should main never wake, unblock it so the test fails instead of
        // hanging.
        for (int i = 0; i < 100 && !stop_waker.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!stop_waker.load()) {
          SDL_Event quit{};
          quit.type = SDL_EVENT_QUIT;
          SDL_PushEvent(&quit);
        }
      });
      return SDL_APP_CONTINUE;
    }
    if (woke && woken_iterate_at == 0.0) woken_iterate_at = NowSec();
    return woke ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
  };
  loop.event = [&](void* /*appstate*/, SDL_Event* event) {
    if (ConsumeWakeEvent(*event)) woke = true;
    if (event->type == SDL_EVENT_QUIT) return SDL_APP_FAILURE;
    return SDL_APP_CONTINUE;
  };
  loop.quit = [&](void* /*appstate*/, SDL_AppResult /*result*/) {
    stop_waker.store(true);
    if (waker.joinable()) waker.join();
    SDL_ResetHint(SDL_HINT_MAIN_CALLBACK_RATE);
    wake.reset();
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
  };

  REQUIRE(RunCallbackLoop(std::move(loop), {}) == 0);
  CHECK(woke);
  // Main was blocked until the wake: the next iterate came after it.
  CHECK(woken_iterate_at >= wake_called_at.load());
  CHECK(woken_iterate_at - blocked_at >= 0.25);
  CHECK(woken_iterate_at - wake_called_at.load() < 0.5);
  REQUIRE(iterations >= 2);
  REQUIRE(iterations <= 3);
  REQUIRE(WakeEventsPushed() - pushed_before == 1);
}

TEST_CASE("Wakes coalesce: at most one wake event is pending", "[wake]") {
  const testing::MainThreadScope main;
  REQUIRE(SDL_InitSubSystem(SDL_INIT_EVENTS));
  {
    auto wake = WakeEventScope::Register();
    REQUIRE(wake);
    const std::uint32_t type = WakeEventType();
    REQUIRE(type != 0);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    const std::uint64_t before = WakeEventsPushed();

    // 100 wakes from four threads at once.
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([] {
        for (int i = 0; i < 25; ++i) WakeMain();
      });
    }
    for (std::thread& thread : threads) thread.join();
    REQUIRE(CountQueued(type) == 1);
    REQUIRE(WakeEventsPushed() - before == 1);
    CHECK(WakePending());

    SDL_Event event{};
    REQUIRE(SDL_PollEvent(&event));
    CHECK(ConsumeWakeEvent(event));
    CHECK_FALSE(WakePending());
    // Another event type is not the wake.
    SDL_Event other{};
    other.type = SDL_EVENT_KEY_DOWN;
    CHECK_FALSE(ConsumeWakeEvent(other));

    // Once consumed, the next wake pushes again.
    WakeMain();
    WakeMain();
    REQUIRE(CountQueued(type) == 1);
    REQUIRE(WakeEventsPushed() - before == 2);
  }
  REQUIRE(WakeEventType() == 0);
  SDL_QuitSubSystem(SDL_INIT_EVENTS);
  SDL_Quit();
}

}  // namespace
}  // namespace se
