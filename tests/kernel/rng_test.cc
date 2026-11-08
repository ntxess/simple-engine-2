// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/rng.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <set>

#include "se_core/guid.h"

namespace se {
namespace {

// Vectors from an independent implementation of the documented mix.
TEST_CASE("rng::At matches the test vectors", "[rng]") {
  REQUIRE(rng::At(0, 0, Guid{0, 0}, 0, 0) == 0x1221d526c863b042ULL);
  REQUIRE(rng::At(42, 1, Guid{0, 1}, 0, 0) == 0x2ecba4ee614c4d1fULL);
  REQUIRE(rng::At(42, 1, Guid{0, 1}, 0, 1) == 0x6af1199262d3a359ULL);
  REQUIRE(rng::At(42, 2, Guid{0, 1}, 0, 0) == 0x47ed56310a77f9dcULL);
  REQUIRE(rng::At(42, 1, Guid{0, 1}, 3, 0) == 0x5b4d49f9df3e2773ULL);
  REQUIRE(rng::At(0xdeadbeef, 123456, Guid{0x1111, 0x2222}, 7, 9) ==
          0x710fe4873c7493a1ULL);
}

TEST_CASE("Every key component changes the draw", "[rng]") {
  const Guid id{5, 6};
  std::set<std::uint64_t> seen;
  seen.insert(rng::At(1, 2, id, 3, 4));
  seen.insert(rng::At(9, 2, id, 3, 4));
  seen.insert(rng::At(1, 9, id, 3, 4));
  seen.insert(rng::At(1, 2, Guid{9, 6}, 3, 4));
  seen.insert(rng::At(1, 2, Guid{5, 9}, 3, 4));
  seen.insert(rng::At(1, 2, id, 9, 4));
  seen.insert(rng::At(1, 2, id, 3, 9));
  REQUIRE(seen.size() == 7);
}

TEST_CASE("Unit takes the top 24 bits", "[rng]") {
  CHECK(rng::Unit(0) == 0.0F);
  CHECK(rng::Unit(~0ULL) == 1.0F - 0x1.0p-24F);
  CHECK(rng::Unit(1ULL << 63) == 0.5F);
  // The low 40 bits are ignored.
  CHECK(rng::Unit((1ULL << 40) - 1) == 0.0F);
}

TEST_CASE("Stream draws are reproducible and in range", "[rng]") {
  rng::Stream a(7, 100, Guid{1, 2}, 0);
  rng::Stream b(7, 100, Guid{1, 2}, 0);
  for (int i = 0; i < 100; ++i) {
    const float u = a.Unit();
    CHECK(u == b.Unit());
    CHECK(u >= 0.0F);
    CHECK(u < 1.0F);
  }
  REQUIRE(a.Draws() == 100);

  rng::Stream c(7, 100, Guid{1, 2}, 0);
  CHECK(c.Next() == rng::At(7, 100, Guid{1, 2}, 0, 0));
  CHECK(c.Next() == rng::At(7, 100, Guid{1, 2}, 0, 1));
}

TEST_CASE("Below is unbiased and below n", "[rng]") {
  rng::Stream s(1234, 0, Guid{0, 42}, 1);
  REQUIRE(s.Below(0) == 0);
  REQUIRE(s.Below(1) == 0);

  constexpr int kBuckets = 10;
  constexpr int kDraws = 100000;
  std::array<int, kBuckets> counts{};
  for (int i = 0; i < kDraws; ++i) {
    const std::uint32_t v = s.Below(kBuckets);
    REQUIRE(v < kBuckets);
    ++counts[v];
  }
  for (const int count : counts) {
    // Expected 10000 per bucket; 4 sigma is about 380.
    REQUIRE(count > 9600);
    REQUIRE(count < 10400);
  }

  // A range that forces rejections: n just above 2^31.
  rng::Stream big(99, 0, Guid{0, 1}, 2);
  constexpr std::uint32_t kLarge = 0x80000001U;
  for (int i = 0; i < 1000; ++i) CHECK(big.Below(kLarge) < kLarge);
}

}  // namespace
}  // namespace se
