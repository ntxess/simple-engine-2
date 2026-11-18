// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/hash.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <entt/core/hashed_string.hpp>
#include <limits>
#include <span>
#include <string_view>

namespace se {
namespace {

// Reference values computed by an independent implementation of the
// documented algorithm (tests/kernel/hash_test.cc pins the constants).
TEST_CASE("Mix64 is the SplitMix64 finalizer", "[hash]") {
  REQUIRE(Mix64(0) == 0);
  REQUIRE(Mix64(1) == 0x5692161d100b05e5ULL);
  REQUIRE(Mix64(0x123456789abcdef0ULL) == 0x9629f58e8ec5b906ULL);
  // The first output of SplitMix64 seeded with 0.
  REQUIRE(Mix64(kHashGamma) == 0xe220a8397b1dcdafULL);
}

TEST_CASE("Hasher matches the documented algorithm", "[hash]") {
  REQUIRE(Hasher().Finish() == 0xe9e0033e3badaf36ULL);

  Hasher one;
  one.AddU64(1);
  REQUIRE(one.Finish() == 0x087cb8f344421b86ULL);

  Hasher abc;
  abc.AddString("abc");
  REQUIRE(abc.Finish() == 0x63a58303d7f58e2cULL);

  Hasher hello;
  hello.AddString("hello, world!");
  REQUIRE(hello.Finish() == 0x4c541d31d2be555bULL);

  Hasher seeded(7);
  seeded.AddU64(1);
  seeded.AddU64(2);
  REQUIRE(seeded.Finish() == 0x42a69b0605565b16ULL);
}

TEST_CASE("Hasher is order and length sensitive", "[hash]") {
  Hasher ab;
  ab.AddU64(1);
  ab.AddU64(2);
  Hasher ba;
  ba.AddU64(2);
  ba.AddU64(1);
  CHECK(ab.Finish() != ba.Finish());

  Hasher short_text;
  short_text.AddString("ab");
  Hasher padded;
  padded.AddString(std::string_view("ab\0", 3));
  CHECK(short_text.Finish() != padded.Finish());

  Hasher empty;
  empty.AddString("");
  CHECK(empty.Finish() != Hasher().Finish());
}

TEST_CASE("Floats hash by canonical bit pattern", "[hash]") {
  CHECK(CanonicalFloatBits(0.0F) == CanonicalFloatBits(-0.0F));
  CHECK(CanonicalDoubleBits(0.0) == CanonicalDoubleBits(-0.0));

  const float quiet = std::numeric_limits<float>::quiet_NaN();
  const float other = std::bit_cast<float>(0xFFC00123U);
  CHECK(CanonicalFloatBits(quiet) == CanonicalFloatBits(other));
  REQUIRE(CanonicalFloatBits(quiet) == 0x7FC00000U);
  const double dquiet = std::numeric_limits<double>::quiet_NaN();
  const double dother = std::bit_cast<double>(0xFFF8000000000123ULL);
  CHECK(CanonicalDoubleBits(dquiet) == CanonicalDoubleBits(dother));

  REQUIRE(CanonicalFloatBits(1.0F) == 0x3F800000U);
  CHECK(CanonicalFloatBits(1.0F) != CanonicalFloatBits(-1.0F));

  Hasher pos;
  pos.AddFloat(0.0F);
  Hasher neg;
  neg.AddFloat(-0.0F);
  CHECK(pos.Finish() == neg.Finish());
}

TEST_CASE("Bytes are packed independently of the host byte order", "[hash]") {
  const std::array<std::byte, 9> bytes = {
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5},
      std::byte{6}, std::byte{7}, std::byte{8}, std::byte{9}};
  Hasher packed;
  packed.AddBytes(bytes);
  Hasher manual;
  manual.AddU64(0x0807060504030201ULL);
  manual.AddU64(0x09);
  manual.AddU64(9);
  CHECK(packed.Finish() == manual.Finish());
}

TEST_CASE("WorldHash folds digests independently of order", "[hash]") {
  std::array<std::uint64_t, 5> digests = {11, 22, 33, 44, 55};
  WorldHash forward;
  for (const std::uint64_t d : digests) forward.Fold(d);
  std::reverse(digests.begin(), digests.end());
  WorldHash backward;
  for (const std::uint64_t d : digests) backward.Fold(d);
  CHECK(forward == backward);
  CHECK(forward.Folded() == backward.Folded());
  REQUIRE(forward.count == 5);

  WorldHash later = forward;
  later.tick = 1;
  CHECK(later.Folded() != forward.Folded());

  WorldHash missing;
  for (std::size_t i = 0; i + 1 < digests.size(); ++i) missing.Fold(digests[i]);
  CHECK(missing.Folded() != forward.Folded());
}

TEST_CASE("Fnv1a32 matches entt::hashed_string", "[hash]") {
  REQUIRE(Fnv1a32("") == 2166136261U);
  REQUIRE(Fnv1a32("a") == 0xe40c292cU);
  constexpr std::string_view kName = "Transform";
  CHECK(Fnv1a32(kName) ==
        entt::hashed_string::value(kName.data(), kName.size()));
}

}  // namespace
}  // namespace se
