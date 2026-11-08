// Copyright 2026 The simple-engine-2 Authors.
//
// The polynomial coefficients and the pi/2 splitting constants are those of
// fdlibm (Copyright (C) 1993 by Sun Microsystems, Inc. "Developed at SunSoft,
// a Sun Microsystems, Inc. business. Permission to use, copy, modify, and
// distribute this software is freely granted, provided that this notice is
// preserved."). The code around them is this engine's.

#include "se_core/detmath.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#include "se_core/hash.h"

namespace se::dm {
namespace {

// sin(x) ~ x + x^3 * (S1 + x^2 * (S2 + ... + x^2 * S6)) on [-pi/4, pi/4].
constexpr double kS1 = -1.66666666666666324348e-01;
constexpr double kS2 = 8.33333333332248946124e-03;
constexpr double kS3 = -1.98412698298579493134e-04;
constexpr double kS4 = 2.75573137070700676789e-06;
constexpr double kS5 = -2.50507602534068634195e-08;
constexpr double kS6 = 1.58969099521155010221e-10;

// cos(x) ~ 1 - x^2/2 + x^4 * (C1 + x^2 * (C2 + ... + x^2 * C6)).
constexpr double kC1 = 4.16666666666666019037e-02;
constexpr double kC2 = -1.38888888888741095749e-03;
constexpr double kC3 = 2.48015872894767294178e-05;
constexpr double kC4 = -2.75573143513906633035e-07;
constexpr double kC5 = 2.08757232129817482790e-09;
constexpr double kC6 = -1.13596475577881948265e-11;

// atan(x) ~ x - x * (x^2 * (A0 + x^4 * (A2 + ...)) + x^4 * (A1 + ...)) for
// |x| <= 7/16.
constexpr double kA0 = 3.33333333333329318027e-01;
constexpr double kA1 = -1.99999999998764832476e-01;
constexpr double kA2 = 1.42857142725034663711e-01;
constexpr double kA3 = -1.11111104054623557880e-01;
constexpr double kA4 = 9.09088713343650656196e-02;
constexpr double kA5 = -7.69187620504482999495e-02;
constexpr double kA6 = 6.66107313738753120669e-02;
constexpr double kA7 = -5.83357013379057348645e-02;
constexpr double kA8 = 4.97687799461593236017e-02;
constexpr double kA9 = -3.65315727442169155270e-02;
constexpr double kA10 = 1.62858201153657823623e-02;

// pi/2 in three 33-bit parts: n * part is exact for |n| < 2^20.
constexpr double kTwoOverPi = 6.36619772367581382433e-01;
constexpr double kPio2Part1 = 1.57079632673412561417e+00;
constexpr double kPio2Part2 = 6.07710050630396597660e-11;
constexpr double kPio2Part3 = 2.02226624871116645580e-21;

// Hi + lo splits of pi/4, pi/2 and pi.
constexpr double kPio4Hi = 7.85398163397448278999e-01;
constexpr double kPio4Lo = 3.06161699786838301793e-17;
constexpr double kPio2Hi = 1.57079632679489655800e+00;
constexpr double kPio2Lo = 6.12323399573676603587e-17;
constexpr double kPiHi = 3.14159265358979311600e+00;
constexpr double kPiLo = 1.22464679914735317720e-16;

// tan(pi/8): below it the atan polynomial applies directly.
constexpr double kTanPio8 = 4.14213562373095034e-01;
// Adding and subtracting 1.5 * 2^52 rounds to the nearest integer (ties to
// even) in round-to-nearest mode, for |v| < 2^51.
constexpr double kRoundMagic = 6755399441055744.0;
// Beyond this the reduction cannot produce a meaningful remainder.
constexpr double kMaxReducible = 1125899906842624.0;  // 2^50

constexpr double kMaxFinite = std::numeric_limits<double>::max();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::uint64_t kSignBit = 0x8000000000000000ULL;

double AbsBits(double x) {
  return std::bit_cast<double>(std::bit_cast<std::uint64_t>(x) & ~kSignBit);
}

bool IsInf(double x) { return AbsBits(x) > kMaxFinite; }

bool SignBit(double x) {
  return (std::bit_cast<std::uint64_t>(x) & kSignBit) != 0;
}

double KernelSin(double x) {
  // Below 2^-27 the cubic term is under half an ulp; returning x also keeps
  // the sign of -0.
  if (AbsBits(x) < 0x1.0p-27) return x;
  const double z = x * x;
  const double v = z * x;
  const double r = kS2 + z * (kS3 + z * (kS4 + z * (kS5 + z * kS6)));
  return x + v * (kS1 + z * r);
}

double KernelCos(double x) {
  const double z = x * x;
  const double r =
      z * (kC1 + z * (kC2 + z * (kC3 + z * (kC4 + z * (kC5 + z * kC6)))));
  const double hz = 0.5 * z;
  const double w = 1.0 - hz;
  return w + (((1.0 - w) - hz) + z * r);
}

struct Reduced {
  double r;
  int quadrant;
};

// x = n * pi/2 + r with |r| <= ~pi/4, quadrant = n mod 4.
Reduced Reduce(double x) {
  if (AbsBits(x) <= kPio4Hi) return {x, 0};
  if (!(AbsBits(x) < kMaxReducible)) return {0.0, 0};
  const double fn = (x * kTwoOverPi + kRoundMagic) - kRoundMagic;
  const double r = ((x - fn * kPio2Part1) - fn * kPio2Part2) - fn * kPio2Part3;
  const auto n = static_cast<std::int64_t>(fn);
  return {r, static_cast<int>(n & 3)};
}

double AtanPoly(double x) {
  const double z = x * x;
  const double w = z * z;
  const double s1 =
      z * (kA0 + w * (kA2 + w * (kA4 + w * (kA6 + w * (kA8 + w * kA10)))));
  const double s2 = w * (kA1 + w * (kA3 + w * (kA5 + w * (kA7 + w * kA9))));
  return x - x * (s1 + s2);
}

// atan(t) for t in [0, 1].
double Atan01(double t) {
  if (t <= kTanPio8) return AtanPoly(t);
  const double u = (t - 1.0) / (t + 1.0);
  const double z = u * u;
  const double w = z * z;
  const double s1 =
      z * (kA0 + w * (kA2 + w * (kA4 + w * (kA6 + w * (kA8 + w * kA10)))));
  const double s2 = w * (kA1 + w * (kA3 + w * (kA5 + w * (kA7 + w * kA9))));
  return kPio4Hi - ((u * (s1 + s2) - kPio4Lo) - u);
}

}  // namespace

double Sin(double x) noexcept {
  if (std::isnan(x)) return x;
  if (IsInf(x)) return kNaN;
  const Reduced red = Reduce(x);
  switch (red.quadrant) {
    case 0:
      return KernelSin(red.r);
    case 1:
      return KernelCos(red.r);
    case 2:
      return -KernelSin(red.r);
    default:
      return -KernelCos(red.r);
  }
}

double Cos(double x) noexcept {
  if (std::isnan(x)) return x;
  if (IsInf(x)) return kNaN;
  const Reduced red = Reduce(x);
  switch (red.quadrant) {
    case 0:
      return KernelCos(red.r);
    case 1:
      return -KernelSin(red.r);
    case 2:
      return -KernelCos(red.r);
    default:
      return KernelSin(red.r);
  }
}

SinCosPair<double> SinCos(double x) noexcept {
  if (std::isnan(x)) return {x, x};
  if (IsInf(x)) return {kNaN, kNaN};
  const Reduced red = Reduce(x);
  const double s = KernelSin(red.r);
  const double c = KernelCos(red.r);
  switch (red.quadrant) {
    case 0:
      return {s, c};
    case 1:
      return {c, -s};
    case 2:
      return {-s, -c};
    default:
      return {-c, s};
  }
}

double Atan2(double y, double x) noexcept {
  if (std::isnan(x) || std::isnan(y)) return x + y;
  const double ax = AbsBits(x);
  const double ay = AbsBits(y);
  double a = 0.0;
  if (IsInf(ax) && IsInf(ay)) {
    a = kPio4Hi + kPio4Lo;
  } else if (ay <= ax) {
    const double t = ax == 0.0 ? 0.0 : ay / ax;
    a = Atan01(t);
  } else {
    const double t = ax / ay;
    a = kPio2Hi - (Atan01(t) - kPio2Lo);
  }
  if (SignBit(x)) a = kPiHi - (a - kPiLo);
  return SignBit(y) ? -a : a;
}

float Sin(float x) noexcept {
  return static_cast<float>(Sin(static_cast<double>(x)));
}

float Cos(float x) noexcept {
  return static_cast<float>(Cos(static_cast<double>(x)));
}

SinCosPair<float> SinCos(float x) noexcept {
  const SinCosPair<double> d = SinCos(static_cast<double>(x));
  return {static_cast<float>(d.sin), static_cast<float>(d.cos)};
}

float Atan2(float y, float x) noexcept {
  return static_cast<float>(
      Atan2(static_cast<double>(y), static_cast<double>(x)));
}

std::uint64_t SelfTestHash() noexcept {
  Hasher hasher;
  constexpr int kSteps = 4096;
  for (int i = 0; i <= kSteps; ++i) {
    // Inputs in [-64, 64], built with exact arithmetic.
    const double x = -64.0 + 128.0 * static_cast<double>(i) / kSteps;
    hasher.AddDouble(Sin(x));
    hasher.AddDouble(Cos(x));
    hasher.AddDouble(Atan2(x, 0.37 * x - 1.5));
    hasher.AddFloat(Sin(static_cast<float>(x)));
    hasher.AddFloat(Cos(static_cast<float>(x)));
    hasher.AddFloat(Atan2(static_cast<float>(x), 1.0F));
    hasher.AddDouble(std::sqrt(AbsBits(x)));
  }
  constexpr std::array<double, 10> kSpecials = {
      0.0, -0.0, kPi, -kPi, kHalfPi, 1e6, -1e6, 1e-300, 7.0e15, 1e300};
  for (const double s : kSpecials) {
    hasher.AddDouble(Sin(s));
    hasher.AddDouble(Cos(s));
    hasher.AddDouble(Atan2(s, -1.0));
    hasher.AddDouble(Atan2(-1.0, s));
  }
  return hasher.Finish();
}

}  // namespace se::dm
