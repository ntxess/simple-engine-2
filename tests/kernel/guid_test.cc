// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/guid.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <unordered_set>

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
