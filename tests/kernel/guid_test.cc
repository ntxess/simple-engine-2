// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/guid.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_set>

#include "se_core/types.h"
#include "se_core/world.h"

namespace se {
namespace {

TEST_CASE("Guid formats as 8-4-4-4-12 hex", "[guid]") {
  const Guid g{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
  REQUIRE(ToString(g) == "01234567-89ab-cdef-fedc-ba9876543210");
  REQUIRE(ToString(Guid{}) == "00000000-0000-0000-0000-000000000000");
  REQUIRE(ToString(Guid{0, 1}) == "00000000-0000-0000-0000-000000000001");
}

TEST_CASE("Guid parses its own format", "[guid]") {
  const Guid g{0xdeadbeef00000001ULL, 0x0000000000abcdefULL};
  const auto parsed = ParseGuid(ToString(g));
  REQUIRE(parsed.has_value());
  CHECK(*parsed == g);
  const auto upper = ParseGuid("DEADBEEF-0000-0001-0000-000000ABCDEF");
  REQUIRE(upper.has_value());
  CHECK(*upper == g);
}

TEST_CASE("Guid parsing rejects malformed text", "[guid]") {
  CHECK_FALSE(ParseGuid("").has_value());
  CHECK_FALSE(ParseGuid("01234567-89ab-cdef-fedc-ba987654321").has_value());
  CHECK_FALSE(ParseGuid("01234567_89ab-cdef-fedc-ba9876543210").has_value());
  CHECK_FALSE(ParseGuid("0123456g-89ab-cdef-fedc-ba9876543210").has_value());
  const auto error = ParseGuid("nope");
  REQUIRE_FALSE(error.has_value());
  CHECK(error.error().code == ErrorCode::kParse);
}

TEST_CASE("Guids order by hi then lo", "[guid]") {
  CHECK(Guid{0, 5} < Guid{1, 0});
  CHECK(Guid{1, 1} < Guid{1, 2});
  CHECK(Guid{2, 2} == Guid{2, 2});
  CHECK(Guid{}.IsNil());
  CHECK_FALSE(Guid{0, 1}.IsNil());
}

TEST_CASE("GuidHash spreads nearby Guids", "[guid]") {
  std::unordered_set<std::size_t> hashes;
  for (std::uint64_t i = 0; i < 1000; ++i) {
    hashes.insert(GuidHash{}(Guid{7, i}));
  }
  REQUIRE(hashes.size() == 1000);
}

}  // namespace
}  // namespace se

namespace se {
namespace {

// Golden DeriveGuid values, from an independent implementation of the rule
// documented in se_core/world.h.
TEST_CASE("DeriveGuid matches the golden values", "[guid][golden]") {
  REQUIRE(World::DeriveGuid(Guid{0, 1}, 0, 0) ==
          Guid{0x4dfa5092a280bd85ULL, 0xda417da197ec3d09ULL});
  REQUIRE(World::DeriveGuid(Guid{1, 2}, 3, 4) ==
          Guid{0xe691a86d66886b7cULL, 0xad21db9518fb7e07ULL});
  REQUIRE(World::DeriveGuid(Guid{0x5E5E, 7}, 100, 0) ==
          Guid{0x9ff99bb90ee97fbcULL, 0x12f2a8b8c9bd2b4cULL});
  REQUIRE(World::DeriveGuid(Guid{0x5E5E, 7}, 100, 1) ==
          Guid{0x84421bbc23f52fd8ULL, 0xa89b743ba5d7de2eULL});
  REQUIRE(World::DeriveGuid(Guid{0x5E5E, 7}, 101, 0) ==
          Guid{0x8b60967cb82afa85ULL, 0x1962941930ca218dULL});
  REQUIRE(World::DeriveGuid(Guid{~0ULL, ~0ULL}, 1ULL << 40, 0xFFFFFFFFU) ==
          Guid{0x139bbe4c3acfb190ULL, 0xa26569e5eea5356bULL});
}

TEST_CASE("Derived Guids are distinct across parent, tick and index",
          "[guid]") {
  std::set<Guid> guids;
  for (std::uint64_t parent = 1; parent <= 4; ++parent) {
    for (Tick tick = 0; tick < 50; ++tick) {
      for (std::uint32_t index = 0; index < 50; ++index) {
        guids.insert(World::DeriveGuid(Guid{0, parent}, tick, index));
      }
    }
  }
  CHECK(guids.size() == std::size_t{10000});  // 4 * 50 * 50
}

}  // namespace
}  // namespace se
