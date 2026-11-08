// Copyright 2026 The simple-engine-2 Authors.
//
// Counter-based random numbers for simulation code. A draw is a pure function
// of (seed, tick, entity Guid, stream, draw index), so results never depend on
// call order, thread count or fork chunking:
//
//   x = Mix64(seed ^ kRngKey)
//   x = Mix64(x + tick * kHashGamma)
//   x = Mix64(x ^ guid.hi)
//   x = Mix64(x + guid.lo * kHashGamma)
//   x = Mix64(x ^ (stream << 32 | draw))
//
// Mix64 is the SplitMix64 finalizer (se_core/hash.h). Test vectors are in
// tests/kernel/rng_test.cc.

#ifndef SE_CORE_RNG_H_
#define SE_CORE_RNG_H_

#include <cstdint>

#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/types.h"

namespace se::rng {

inline constexpr std::uint64_t kRngKey = 0x5EED5EED0123ABCDULL;

// One 64-bit draw.
[[nodiscard]] constexpr std::uint64_t At(std::uint64_t seed, Tick tick,
                                         const Guid& id, std::uint32_t stream,
                                         std::uint32_t draw) noexcept {
  std::uint64_t x = Mix64(seed ^ kRngKey);
  x = Mix64(x + tick * kHashGamma);
  x = Mix64(x ^ id.hi);
  x = Mix64(x + id.lo * kHashGamma);
  x = Mix64(x ^ ((static_cast<std::uint64_t>(stream) << 32) | draw));
  return x;
}

// A float in [0, 1) from the top 24 bits: every value is exact.
[[nodiscard]] constexpr float Unit(std::uint64_t bits) noexcept {
  return static_cast<float>(bits >> 40) * 0x1.0p-24F;
}

// Successive draws for one (seed, tick, entity, stream).
class Stream {
 public:
  constexpr Stream(std::uint64_t seed, Tick tick, const Guid& id,
                   std::uint32_t stream) noexcept
      : seed_(seed), tick_(tick), id_(id), stream_(stream) {}

  [[nodiscard]] constexpr std::uint64_t Next() noexcept {
    return At(seed_, tick_, id_, stream_, draw_++);
  }

  // A float in [0, 1).
  [[nodiscard]] constexpr float Unit() noexcept { return rng::Unit(Next()); }

  // An unbiased integer in [0, n), by Lemire's multiply-and-reject method;
  // rejected draws consume further draw indices. Below(0) is 0.
  [[nodiscard]] constexpr std::uint32_t Below(std::uint32_t n) noexcept {
    if (n == 0) return 0;
    std::uint64_t m = (Next() >> 32) * n;
    auto low = static_cast<std::uint32_t>(m);
    if (low < n) {
      const std::uint32_t threshold = (0U - n) % n;
      while (low < threshold) {
        m = (Next() >> 32) * n;
        low = static_cast<std::uint32_t>(m);
      }
    }
    return static_cast<std::uint32_t>(m >> 32);
  }

  // The number of draws taken so far.
  [[nodiscard]] constexpr std::uint32_t Draws() const noexcept { return draw_; }

 private:
  std::uint64_t seed_;
  Tick tick_;
  Guid id_;
  std::uint32_t stream_;
  std::uint32_t draw_ = 0;
};

}  // namespace se::rng

#endif  // SE_CORE_RNG_H_
