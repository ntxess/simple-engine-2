// Copyright 2026 The simple-engine-2 Authors.
//
// Small value types shared by the engine: 2D vectors, rectangles, colours and
// the sim tick. Plain aggregates, so the component registry can reflect them.

#ifndef SE_CORE_TYPES_H_
#define SE_CORE_TYPES_H_

#include <cstdint>

namespace se {

// A simulation tick number. Tick 0 is the state after loading; the first
// stepped tick is 1.
using Tick = std::uint64_t;

struct Vec2 {
  float x = 0.0F;
  float y = 0.0F;

  friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
};

constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept {
  return {a.x + b.x, a.y + b.y};
}
constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept {
  return {a.x - b.x, a.y - b.y};
}
constexpr Vec2 operator*(Vec2 a, float s) noexcept {
  return {a.x * s, a.y * s};
}
constexpr Vec2 operator*(float s, Vec2 a) noexcept {
  return {a.x * s, a.y * s};
}

// An axis-aligned rectangle: top-left corner and size.
struct Rect {
  float x = 0.0F;
  float y = 0.0F;
  float w = 0.0F;
  float h = 0.0F;

  friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

// An 8-bit-per-channel RGBA colour.
struct Rgba8 {
  std::uint8_t r = 255;
  std::uint8_t g = 255;
  std::uint8_t b = 255;
  std::uint8_t a = 255;

  friend constexpr bool operator==(const Rgba8&, const Rgba8&) = default;
};

}  // namespace se

#endif  // SE_CORE_TYPES_H_
