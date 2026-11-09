// Copyright 2026 The simple-engine-2 Authors.
//
// The event filter and the Inbox under SDL's real main-callback driver: an
// engine thread pushes LOW_MEMORY and TERMINATING while main iterates. Run
// under TSan in WSL (linux-clang-tsan).

#include "se_platform/inbox.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "platform/support/callback_loop.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::CallbackLoop;
using testing::NowSec;
using testing::RunCallbackLoop;

constexpr int kUserEvents = 200;
constexpr double kTimeoutSec = 10.0;

// What the test's own event watcher saw: it stands in for SDL's main-
// callback watcher, which would dispatch immediate events on the pushing
// thread.
struct WatchRecord {
  std::atomic<int> immediate_off_main{0};
  std::atomic<int> immediate_on_main{0};
};

bool SDLCALL TestWatcher(void* userdata, SDL_Event* event) {
  auto* record = static_cast<WatchRecord*>(userdata);
  if (IsImmediateDispatchEvent(event->type)) {
    if (SDL_IsMainThread()) {
      record->immediate_on_main.fetch_add(1);
    } else {
      record->immediate_off_main.fetch_add(1);
    }
  }
  return true;
}

TEST_CASE("The six immediate-dispatch event types", "[inbox]") {
  CHECK(IsImmediateDispatchEvent(SDL_EVENT_TERMINATING));
  CHECK(IsImmediateDispatchEvent(SDL_EVENT_LOW_MEMORY));
  CHECK(IsImmediateDispatchEvent(SDL_EVENT_WILL_ENTER_BACKGROUND));
  CHECK(IsImmediateDispatchEvent(SDL_EVENT_DID_ENTER_BACKGROUND));
  CHECK(IsImmediateDispatchEvent(SDL_EVENT_WILL_ENTER_FOREGROUND));
  CHECK(IsImmediateDispatchEvent(SDL_EVENT_DID_ENTER_FOREGROUND));
  CHECK_FALSE(IsImmediateDispatchEvent(SDL_EVENT_QUIT));
  CHECK_FALSE(IsImmediateDispatchEvent(SDL_EVENT_USER));
  CHECK_FALSE(IsImmediateDispatchEvent(SDL_EVENT_KEY_DOWN));
}

TEST_CASE("A full inbox drops and counts", "[inbox]") {
  const testing::MainThreadScope main;
  Inbox inbox;
  SDL_Event event{};
  event.type = SDL_EVENT_LOW_MEMORY;
  for (std::size_t i = 0; i < Inbox::kCapacity; ++i) {
    REQUIRE(inbox.Push(event));
  }
  CHECK_FALSE(inbox.Push(event));
  CHECK(inbox.Diverted() == Inbox::kCapacity);
  REQUIRE(inbox.Dropped() == 1);
  CHECK(inbox.TryTake().size() == Inbox::kCapacity);
  CHECK(inbox.TryTake().empty());
  CHECK(inbox.Push(event));
}

TEST_CASE(
    "Immediate events pushed off main reach main through the inbox, and "
    "regular events keep their order",
    "[inbox]") {
  const testing::MainThreadScope main;
  Inbox inbox;
  std::optional<EventFilter> filter;
  WatchRecord watch;
  std::uint32_t user_type = 0;
  std::thread engine_thread;
  std::atomic<bool> pushed_all{false};

  std::vector<std::uint32_t> via_inbox;
  std::vector<std::int32_t> user_codes;
  int immediate_via_queue = 0;
  int events_off_main = 0;
  bool timed_out = false;
  double start = 0.0;

  CallbackLoop loop;
  loop.init = [&](void** /*appstate*/, int /*argc*/, char** /*argv*/) {
    AssertMainThread();
    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) return SDL_APP_FAILURE;
    auto installed = EventFilter::Install(inbox);
    if (!installed) return SDL_APP_FAILURE;
    filter.emplace(std::move(*installed));
    if (!SDL_AddEventWatch(&TestWatcher, &watch)) return SDL_APP_FAILURE;
    user_type = SDL_RegisterEvents(1);
    start = NowSec();
    return user_type != 0 ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
  };
  loop.iterate = [&](void* /*appstate*/) {
    AssertMainThread();
    if (!engine_thread.joinable()) {
      // SDL's own watcher is installed once init returns; start pushing
      // while main iterates.
      engine_thread = std::thread([&] {
        for (int i = 0; i < kUserEvents; ++i) {
          SDL_Event event{};
          event.type = user_type;
          event.user.code = i;
          SDL_PushEvent(&event);
          if (i == kUserEvents / 3) {
            SDL_Event low{};
            low.type = SDL_EVENT_LOW_MEMORY;
            SDL_PushEvent(&low);
          }
          if (i == 2 * kUserEvents / 3) {
            SDL_Event terminating{};
            terminating.type = SDL_EVENT_TERMINATING;
            SDL_PushEvent(&terminating);
          }
          if (i % 16 == 0) std::this_thread::yield();
        }
        pushed_all.store(true);
      });
    }
    for (const SDL_Event& event : inbox.TryTake()) {
      via_inbox.push_back(event.type);
    }
    const bool done =
        pushed_all.load() && via_inbox.size() == 2 &&
        user_codes.size() == static_cast<std::size_t>(kUserEvents);
    if (done) return SDL_APP_SUCCESS;
    if (NowSec() - start > kTimeoutSec) {
      timed_out = true;
      return SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
  };
  loop.event = [&](void* /*appstate*/, SDL_Event* event) {
    if (!SDL_IsMainThread()) ++events_off_main;
    if (event->type == user_type) user_codes.push_back(event->user.code);
    if (IsImmediateDispatchEvent(event->type)) ++immediate_via_queue;
    return SDL_APP_CONTINUE;
  };
  loop.quit = [&](void* /*appstate*/, SDL_AppResult /*result*/) {
    if (engine_thread.joinable()) engine_thread.join();
    SDL_RemoveEventWatch(&TestWatcher, &watch);
    filter.reset();
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
  };

  REQUIRE(RunCallbackLoop(std::move(loop), {}) == 0);
  CHECK_FALSE(timed_out);
  // Both immediate events came through the inbox, in order, and never
  // through SDL's queue or a watcher on the pushing thread.
  REQUIRE(via_inbox == std::vector<std::uint32_t>{SDL_EVENT_LOW_MEMORY,
                                                  SDL_EVENT_TERMINATING});
  REQUIRE(watch.immediate_off_main.load() == 0);
  REQUIRE(immediate_via_queue == 0);
  REQUIRE(events_off_main == 0);
  REQUIRE(inbox.Diverted() == 2);
  REQUIRE(inbox.Dropped() == 0);
  // Regular events passed the filter untouched and kept their order.
  REQUIRE(user_codes.size() == static_cast<std::size_t>(kUserEvents));
  for (int i = 0; i < kUserEvents; ++i) {
    REQUIRE(user_codes[static_cast<std::size_t>(i)] == i);
  }
}

// The control: without the engine's filter, SDL's main-callback watcher
// dispatches a LOW_MEMORY pushed off main on the pushing thread itself.
TEST_CASE("Without the filter SDL dispatches immediate events off main",
          "[inbox]") {
  const testing::MainThreadScope main;
  std::thread engine_thread;
  std::atomic<int> low_memory_off_main{0};
  std::atomic<int> low_memory_on_main{0};
  std::atomic<bool> pushed{false};
  double start = 0.0;

  CallbackLoop loop;
  loop.init = [&](void** /*appstate*/, int /*argc*/, char** /*argv*/) {
    start = NowSec();
    return SDL_InitSubSystem(SDL_INIT_EVENTS) ? SDL_APP_CONTINUE
                                              : SDL_APP_FAILURE;
  };
  loop.iterate = [&](void* /*appstate*/) {
    if (!engine_thread.joinable()) {
      engine_thread = std::thread([&] {
        SDL_Event low{};
        low.type = SDL_EVENT_LOW_MEMORY;
        SDL_PushEvent(&low);
        pushed.store(true);
      });
    }
    const bool seen =
        low_memory_off_main.load() + low_memory_on_main.load() > 0;
    if ((pushed.load() && seen) || NowSec() - start > kTimeoutSec) {
      return SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
  };
  // May run on the engine thread: only atomics here.
  loop.event = [&](void* /*appstate*/, SDL_Event* event) {
    if (event->type == SDL_EVENT_LOW_MEMORY) {
      if (SDL_IsMainThread()) {
        low_memory_on_main.fetch_add(1);
      } else {
        low_memory_off_main.fetch_add(1);
      }
    }
    return SDL_APP_CONTINUE;
  };
  loop.quit = [&](void* /*appstate*/, SDL_AppResult /*result*/) {
    if (engine_thread.joinable()) engine_thread.join();
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
  };

  REQUIRE(RunCallbackLoop(std::move(loop), {}) == 0);
  REQUIRE(low_memory_off_main.load() == 1);
  REQUIRE(low_memory_on_main.load() == 0);
}

TEST_CASE("Immediate events pushed on main pass the filter", "[inbox]") {
  const testing::MainThreadScope main;
  REQUIRE(SDL_InitSubSystem(SDL_INIT_EVENTS));
  Inbox inbox;
  {
    auto filter = EventFilter::Install(inbox);
    REQUIRE(filter);
    // A second filter would replace the first.
    CHECK_FALSE(EventFilter::Install(inbox));
    SDL_Event low{};
    low.type = SDL_EVENT_LOW_MEMORY;
    REQUIRE(SDL_PushEvent(&low));
    SDL_Event event{};
    REQUIRE(SDL_PollEvent(&event));
    CHECK(event.type == SDL_EVENT_LOW_MEMORY);
    REQUIRE(inbox.Diverted() == 0);
  }
  SDL_EventFilter current = nullptr;
  void* userdata = nullptr;
  CHECK_FALSE(SDL_GetEventFilter(&current, &userdata));
  SDL_QuitSubSystem(SDL_INIT_EVENTS);
  SDL_Quit();
}

}  // namespace
}  // namespace se
