// Copyright 2026 The simple-engine-2 Authors.
//
// Deterministic trigonometry for simulation code (design change 20). The CRT
// functions may differ between compilers, CRT versions and CPUs (the MSVC
// x64 CRT picks FMA3 paths by CPU), so simulation code calls these instead;
// the banned-API scan rejects the CRT ones in DETERMINISTIC modules.
//
// Only + - * / and std::sqrt are used, in a translation unit compiled with
// the determinism flags (/fp:precise, no contraction):
//   - Sin and Cos: Cody-Waite reduction by pi/2 in three parts to
//     [-pi/4, pi/4], then minimax polynomials (the fdlibm kernels);
//   - Atan2: atan on [0, 1] (split at tan(pi/8), above it
//     atan(t) = pi/4 + atan((t - 1) / (t + 1))) and an octant fix-up.
// The float overloads compute in double and round once.
//
// Accuracy: within 2 ulp of the correctly rounded result for |x| <= 1e6
// (tests/kernel/detmath_test.cc). Results are deterministic for every input;
// above 2^50 the reduction is no longer meaningful. NaN in gives NaN out.

#ifndef SE_CORE_DETMATH_H_
#define SE_CORE_DETMATH_H_

#include <cstdint>

namespace se::dm {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 6.28318530717958647692;
inline constexpr double kHalfPi = 1.57079632679489661923;

template <class T>
struct SinCosPair {
  T sin;
  T cos;
};

[[nodiscard]] double Sin(double x) noexcept;
[[nodiscard]] double Cos(double x) noexcept;
[[nodiscard]] SinCosPair<double> SinCos(double x) noexcept;
[[nodiscard]] double Atan2(double y, double x) noexcept;

[[nodiscard]] float Sin(float x) noexcept;
[[nodiscard]] float Cos(float x) noexcept;
[[nodiscard]] SinCosPair<float> SinCos(float x) noexcept;
[[nodiscard]] float Atan2(float y, float x) noexcept;

// A hash of these functions' results over a fixed input set. Replay headers
// carry it; a different value means the math differs from the recording.
[[nodiscard]] std::uint64_t SelfTestHash() noexcept;

}  // namespace se::dm

#endif  // SE_CORE_DETMATH_H_
