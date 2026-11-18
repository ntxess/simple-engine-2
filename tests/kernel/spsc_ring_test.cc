// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/spsc_ring.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace se {
namespace {

struct Item {
  std::uint64_t value = 0;
  std::uint64_t check = 0;
};

TEST_CASE("SpscRing delivers a million items once and in order",
          "[spsc_ring][threads]") {
  constexpr std::uint64_t kItems = 1'000'000;
  SpscRing<Item> ring(1024);
  std::uint64_t producer_full = 0;
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      const Item item{i, ~i};
      while (!ring.TryPush(item)) {
        ++producer_full;
        std::this_thread::yield();
      }
    }
  });
  std::uint64_t next = 0;
  bool in_order = true;
  while (next < kItems) {
    const std::size_t popped = ring.PopAll([&](const Item& item) {
      in_order = in_order && item.value == next && item.check == ~next;
      ++next;
    });
    if (popped == 0) {
      if (const std::optional<Item> item = ring.TryPop(); item.has_value()) {
        in_order = in_order && item->value == next && item->check == ~next;
        ++next;
      } else {
        std::this_thread::yield();
      }
    }
  }
  producer.join();
  REQUIRE(in_order);
  REQUIRE(next == kItems);
  CHECK_FALSE(ring.TryPop().has_value());
  REQUIRE(ring.SizeApprox() == 0);
}

TEST_CASE("A full SpscRing refuses the new item and the caller counts it",
          "[spsc_ring]") {
  SpscRing<std::uint32_t> ring(8);
  REQUIRE(ring.Capacity() == 8);
  std::uint32_t dropped = 0;
  for (std::uint32_t i = 0; i < 12; ++i) {
    if (!ring.TryPush(i)) ++dropped;
  }
  REQUIRE(dropped == 4);
  REQUIRE(ring.SizeApprox() == 8);
  // The oldest items survive; the refused ones never appear.
  std::vector<std::uint32_t> seen;
  ring.PopAll([&](std::uint32_t v) { seen.push_back(v); });
  REQUIRE(seen == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 6, 7});
  // Space again after the pop.
  REQUIRE(ring.TryPush(100));
  REQUIRE(ring.TryPop() == std::optional<std::uint32_t>(100));
}

TEST_CASE("SpscRing indices wrap past the capacity", "[spsc_ring]") {
  SpscRing<std::uint64_t> ring(4);
  for (std::uint64_t round = 0; round < 1000; ++round) {
    REQUIRE(ring.TryPush(round));
    REQUIRE(ring.TryPush(round + 1));
    REQUIRE(ring.TryPop() == std::optional<std::uint64_t>(round));
    REQUIRE(ring.TryPop() == std::optional<std::uint64_t>(round + 1));
  }
  REQUIRE_FALSE(ring.TryPop().has_value());
}

}  // namespace
}  // namespace se
