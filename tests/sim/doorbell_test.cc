// Copyright 2026 The simple-engine-2 Authors.
//
// Doorbell: a ring wakes the waiter, a ring before the wait is never lost,
// and a stop request ends the wait (its stop_callback rings).

#include "se_core/doorbell.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <stop_token>
#include <thread>

namespace se {
namespace {

TEST_CASE("A doorbell ring wakes the waiter", "[doorbell][threads]") {
  Doorbell bell;
  std::atomic<int> work{0};
  std::atomic<bool> done{false};
  std::thread waiter([&] {
    const std::stop_source never;
    int seen_work = 0;
    while (seen_work < 1000) {
      const std::uint32_t seen = bell.Seen();
      const int now = work.load(std::memory_order_acquire);
      if (now > seen_work) {
        seen_work = now;
        continue;
      }
      bell.Wait(seen, never.get_token());
    }
    done.store(true, std::memory_order_release);
  });
  for (int i = 1; i <= 1000; ++i) {
    work.store(i, std::memory_order_release);
    bell.Ring();
    if (i % 100 == 0) std::this_thread::yield();
  }
  waiter.join();
  REQUIRE(done.load());
}

TEST_CASE("A ring between Seen and Wait is not lost", "[doorbell]") {
  Doorbell bell;
  const std::uint32_t seen = bell.Seen();
  bell.Ring();
  const std::stop_source never;
  // Returns at once: the sequence moved.
  bell.Wait(seen, never.get_token());
  REQUIRE(bell.Seen() == seen + 1);
}

TEST_CASE("A stop request ends a doorbell wait", "[doorbell][threads]") {
  Doorbell bell;
  std::stop_source stop;
  std::atomic<bool> returned{false};
  std::thread waiter([&] {
    bell.Wait(bell.Seen(), stop.get_token());
    returned.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  stop.request_stop();
  waiter.join();
  REQUIRE(returned.load());
  // Already stopped: returns at once.
  bell.Wait(bell.Seen(), stop.get_token());
}

}  // namespace
}  // namespace se
