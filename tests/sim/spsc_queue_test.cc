// Copyright 2026 The simple-engine-2 Authors.
//
// SpscQueue: move-only values cross once and in order between two threads
// (under TSan in linux-clang-tsan), a full queue refuses and leaves the
// value with the caller, Front peeks without popping, and what is left is
// destroyed with the queue.

#include "se_core/spsc_queue.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace se {
namespace {

TEST_CASE("SpscQueue moves a million owned values across once and in order",
          "[spsc_queue][threads]") {
  constexpr std::uint64_t kItems = 1'000'000;
  SpscQueue<std::unique_ptr<std::uint64_t>> queue(256);
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      auto value = std::make_unique<std::uint64_t>(i);
      while (!queue.TryPush(std::move(value))) std::this_thread::yield();
    }
  });
  std::uint64_t next = 0;
  bool in_order = true;
  while (next < kItems) {
    if (std::unique_ptr<std::uint64_t>* front = queue.Front()) {
      in_order = in_order && *front != nullptr && **front == next;
      queue.Pop();
      ++next;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  REQUIRE(in_order);
  REQUIRE(queue.Front() == nullptr);
  REQUIRE(queue.SizeApprox() == 0);
}

TEST_CASE("A full SpscQueue refuses and the value stays with the caller",
          "[spsc_queue]") {
  SpscQueue<std::unique_ptr<int>> queue(4);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(queue.TryPush(std::make_unique<int>(i)));
  }
  auto refused = std::make_unique<int>(99);
  REQUIRE_FALSE(queue.TryPush(std::move(refused)));
  // Not moved from: the caller still owns it.
  REQUIRE(refused != nullptr);
  REQUIRE(*refused == 99);
  std::optional<std::unique_ptr<int>> first = queue.TryPop();
  REQUIRE(first.has_value());
  REQUIRE(**first == 0);
  REQUIRE(queue.TryPush(std::move(refused)));
  REQUIRE(queue.SizeApprox() == 4);
}

TEST_CASE("SpscQueue::Front peeks; only Pop removes", "[spsc_queue]") {
  SpscQueue<std::uint32_t> queue(8);
  REQUIRE(queue.Front() == nullptr);
  REQUIRE(queue.TryPush(std::uint32_t{7}));
  REQUIRE(queue.TryPush(std::uint32_t{8}));
  REQUIRE(*queue.Front() == 7);
  REQUIRE(*queue.Front() == 7);
  queue.Pop();
  REQUIRE(*queue.Front() == 8);
  queue.Pop();
  REQUIRE(queue.Front() == nullptr);
  // Indices wrap past the capacity.
  for (std::uint32_t round = 0; round < 100; ++round) {
    REQUIRE(queue.TryPush(round));
    REQUIRE(queue.TryPop() == std::optional<std::uint32_t>(round));
  }
}

TEST_CASE("Values left in an SpscQueue are destroyed with it", "[spsc_queue]") {
  auto counter = std::make_shared<int>(0);
  {
    SpscQueue<std::shared_ptr<int>> queue(8);
    for (int i = 0; i < 5; ++i) {
      std::shared_ptr<int> copy = counter;
      REQUIRE(queue.TryPush(std::move(copy)));
    }
    REQUIRE(counter.use_count() == 6);
    queue.Pop();
    REQUIRE(counter.use_count() == 5);
  }
  REQUIRE(counter.use_count() == 1);
}

}  // namespace
}  // namespace se
