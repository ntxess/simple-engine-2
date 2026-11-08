// Copyright 2026 The simple-engine-2 Authors.
//
// Deterministic hashing for world state (ADR-011).
//
// Hasher is a streaming 64-bit hash with fixed constants, so a value hashes
// the same on every platform, compiler and build:
//
//   start   state = kHashSeed (or a caller seed), count = 0
//   word w  state = Mix64(RotL(state, 23) ^ Mix64(w + kHashGamma)), ++count
//   finish  Mix64(state ^ (count * kHashGamma))
//
// Mix64 is the SplitMix64 finalizer. Bytes are packed into little-endian
// words regardless of the host byte order, and the byte length is appended.
// Floats are hashed by bit pattern after canonicalising -0 to +0 and every
// NaN to the default quiet NaN, so equal values always hash equal.
//
// WorldHash folds per-entity digests by sum and xor, so it does not depend on
// entity storage order or fork chunking.

#ifndef SE_CORE_HASH_H_
#define SE_CORE_HASH_H_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "se_core/types.h"

namespace se {

inline constexpr std::uint64_t kHashSeed = 0x243F6A8885A308D3ULL;  // pi
inline constexpr std::uint64_t kHashGamma = 0x9E3779B97F4A7C15ULL;
inline constexpr std::uint64_t kMixMulA = 0xBF58476D1CE4E5B9ULL;
inline constexpr std::uint64_t kMixMulB = 0x94D049BB133111EBULL;

// The SplitMix64 finalizer: a bijection with full avalanche.
[[nodiscard]] constexpr std::uint64_t Mix64(std::uint64_t x) noexcept {
  x ^= x >> 30;
  x *= kMixMulA;
  x ^= x >> 27;
  x *= kMixMulB;
  x ^= x >> 31;
  return x;
}

// 32-bit FNV-1a; used for stable names (component name ids). Matches
// entt::hashed_string.
[[nodiscard]] constexpr std::uint32_t Fnv1a32(std::string_view text) noexcept {
  std::uint32_t h = 2166136261U;
  for (const char c : text) {
    h ^= static_cast<std::uint8_t>(c);
    h *= 16777619U;
  }
  return h;
}

// Canonical bit patterns for hashing floating-point values.
[[nodiscard]] constexpr std::uint32_t CanonicalFloatBits(float v) noexcept {
  if (v == 0.0F) return 0;         // +0 and -0
  if (v != v) return 0x7FC00000U;  // every NaN
  return std::bit_cast<std::uint32_t>(v);
}
[[nodiscard]] constexpr std::uint64_t CanonicalDoubleBits(double v) noexcept {
  if (v == 0.0) return 0;
  if (v != v) return 0x7FF8000000000000ULL;
  return std::bit_cast<std::uint64_t>(v);
}

class Hasher {
 public:
  constexpr Hasher() noexcept = default;
  constexpr explicit Hasher(std::uint64_t seed) noexcept : state_(seed) {}

  constexpr void AddU64(std::uint64_t v) noexcept {
    state_ = Mix64(std::rotl(state_, 23) ^ Mix64(v + kHashGamma));
    ++count_;
  }
  constexpr void AddI64(std::int64_t v) noexcept {
    AddU64(static_cast<std::uint64_t>(v));
  }
  constexpr void AddBool(bool v) noexcept { AddU64(v ? 1 : 0); }
  constexpr void AddFloat(float v) noexcept { AddU64(CanonicalFloatBits(v)); }
  constexpr void AddDouble(double v) noexcept {
    AddU64(CanonicalDoubleBits(v));
  }
  void AddBytes(std::span<const std::byte> bytes) noexcept;
  void AddString(std::string_view text) noexcept;

  [[nodiscard]] constexpr std::uint64_t Finish() const noexcept {
    return Mix64(state_ ^ (count_ * kHashGamma));
  }

 private:
  std::uint64_t state_ = kHashSeed;
  std::uint64_t count_ = 0;
};

// The order-independent hash of a World after a tick (ADR-011): per-entity
// digests (Guid plus hashed component bytes) folded by sum and xor.
struct WorldHash {
  std::uint64_t sum = 0;
  std::uint64_t xr = 0;
  std::uint32_t count = 0;
  Tick tick = 0;

  // Adds one entity digest.
  constexpr void Fold(std::uint64_t digest) noexcept {
    sum += digest;
    xr ^= digest;
    ++count;
  }

  // One word for per-tick sequences and goldens.
  [[nodiscard]] constexpr std::uint64_t Folded() const noexcept {
    return Mix64(sum ^ Mix64(xr ^ Mix64(count ^ Mix64(tick + kHashGamma))));
  }

  friend constexpr bool operator==(const WorldHash&,
                                   const WorldHash&) = default;
};

}  // namespace se

#endif  // SE_CORE_HASH_H_
