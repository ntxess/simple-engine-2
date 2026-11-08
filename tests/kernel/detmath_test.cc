// Copyright 2026 The simple-engine-2 Authors.
//
// se::dm against the CRT (error bounds), bit-exact goldens, the self-test
// hash, and the FMA3 switch.

#include "se_core/detmath.h"

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "se_core/fp.h"
#include "support/golden.h"

namespace se {
namespace {

// Distance in units in the last place between two finite doubles.
std::uint64_t UlpDistance(double a, double b) {
  auto key = [](double v) {
    const auto bits = std::bit_cast<std::int64_t>(v);
    return bits < 0 ? std::numeric_limits<std::int64_t>::min() - bits : bits;
  };
  const std::int64_t ka = key(a);
  const std::int64_t kb = key(b);
  return ka > kb ? static_cast<std::uint64_t>(ka - kb)
                 : static_cast<std::uint64_t>(kb - ka);
}

std::uint32_t UlpDistanceF(float a, float b) {
  auto key = [](float v) {
    const auto bits = std::bit_cast<std::int32_t>(v);
    return bits < 0 ? std::numeric_limits<std::int32_t>::min() - bits : bits;
  };
  const std::int32_t ka = key(a);
  const std::int32_t kb = key(b);
  return ka > kb ? static_cast<std::uint32_t>(ka - kb)
                 : static_cast<std::uint32_t>(kb - ka);
}

// Inputs spread over [-range, range] by a fixed LCG (no CRT randomness).
std::vector<double> Inputs(double range, int count) {
  std::vector<double> xs;
  xs.reserve(static_cast<std::size_t>(count));
  std::uint64_t state = 0x1234567ULL;
  for (int i = 0; i < count; ++i) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    const double unit = static_cast<double>(state >> 11) * 0x1.0p-53;
    xs.push_back((2.0 * unit - 1.0) * range);
  }
  return xs;
}

// Within 2 ulp of the CRT, or within 1e-20 absolute where the result is
// tiny (near multiples of pi/2 the remainder loses relative accuracy).
bool Close(double got, double want) {
  return UlpDistance(got, want) <= 2 || std::abs(got - want) <= 1e-20;
}

TEST_CASE("Sin and Cos stay within 2 ulp of the CRT", "[detmath]") {
  for (const double range : {1.0, 10.0, 1000.0, 1e6}) {
    std::uint64_t worst_sin = 0;
    std::uint64_t worst_cos = 0;
    for (const double x : Inputs(range, 20000)) {
      const double s = dm::Sin(x);
      const double c = dm::Cos(x);
      INFO("x = " << x << " range " << range);
      REQUIRE(Close(s, std::sin(x)));
      REQUIRE(Close(c, std::cos(x)));
      if (std::abs(std::sin(x)) > 1e-10) {
        worst_sin = std::max(worst_sin, UlpDistance(s, std::sin(x)));
      }
      if (std::abs(std::cos(x)) > 1e-10) {
        worst_cos = std::max(worst_cos, UlpDistance(c, std::cos(x)));
      }
    }
    REQUIRE(worst_sin <= 2);
    REQUIRE(worst_cos <= 2);
  }
}

TEST_CASE("Atan2 stays within 2 ulp of the CRT", "[detmath]") {
  const std::vector<double> ys = Inputs(100.0, 3000);
  const std::vector<double> xs = Inputs(0.01, 3000);
  for (std::size_t i = 0; i < ys.size(); ++i) {
    for (const double x : {xs[i], ys[(i * 7) % ys.size()], -ys[i] * 3.0}) {
      const double y = ys[i];
      INFO("y = " << y << " x = " << x);
      REQUIRE(UlpDistance(dm::Atan2(y, x), std::atan2(y, x)) <= 2);
    }
  }
}

TEST_CASE("Atan2 handles zeros, infinities and NaN like the CRT", "[detmath]") {
  const double inf = std::numeric_limits<double>::infinity();
  const std::vector<double> specials = {0.0, -0.0, 1.0,    -1.0,
                                        inf, -inf, 1e-310, -1e300};
  for (const double y : specials) {
    for (const double x : specials) {
      INFO("y = " << y << " x = " << x);
      const double got = dm::Atan2(y, x);
      const double want = std::atan2(y, x);
      CHECK(std::signbit(got) == std::signbit(want));
      REQUIRE(UlpDistance(got, want) <= 1);
    }
  }
  CHECK(std::isnan(dm::Atan2(std::nan(""), 1.0)));
  CHECK(std::isnan(dm::Atan2(1.0, std::nan(""))));
  CHECK(std::isnan(dm::Sin(inf)));
  CHECK(std::isnan(dm::Cos(-inf)));
  CHECK(std::isnan(dm::Sin(std::nan(""))));
  CHECK(std::signbit(dm::Sin(-0.0)));
}

TEST_CASE("Float overloads round the double result once", "[detmath]") {
  for (const double x : Inputs(50.0, 20000)) {
    const auto xf = static_cast<float>(x);
    const auto xd = static_cast<double>(xf);
    INFO("x = " << xf);
    REQUIRE(UlpDistanceF(dm::Sin(xf), static_cast<float>(std::sin(xd))) <= 1);
    REQUIRE(UlpDistanceF(dm::Cos(xf), static_cast<float>(std::cos(xd))) <= 1);
    REQUIRE(UlpDistanceF(dm::Atan2(xf, 0.5F),
                         static_cast<float>(std::atan2(xd, 0.5))) <= 1);
  }
}

TEST_CASE("SinCos equals Sin and Cos bit for bit", "[detmath]") {
  for (const double x : Inputs(1e4, 5000)) {
    const dm::SinCosPair<double> sc = dm::SinCos(x);
    CHECK(std::bit_cast<std::uint64_t>(sc.sin) ==
          std::bit_cast<std::uint64_t>(dm::Sin(x)));
    CHECK(std::bit_cast<std::uint64_t>(sc.cos) ==
          std::bit_cast<std::uint64_t>(dm::Cos(x)));
    const auto xf = static_cast<float>(x);
    const dm::SinCosPair<float> scf = dm::SinCos(xf);
    CHECK(scf.sin == dm::Sin(xf));
    CHECK(scf.cos == dm::Cos(xf));
  }
}

std::vector<std::uint64_t> BitPatterns() {
  std::vector<std::uint64_t> bits;
  for (const double x : Inputs(1e3, 256)) {
    bits.push_back(std::bit_cast<std::uint64_t>(dm::Sin(x)));
    bits.push_back(std::bit_cast<std::uint64_t>(dm::Cos(x)));
    bits.push_back(std::bit_cast<std::uint64_t>(dm::Atan2(x, 1.0 - x)));
    bits.push_back(
        std::bit_cast<std::uint32_t>(dm::Sin(static_cast<float>(x))));
  }
  bits.push_back(dm::SelfTestHash());
  return bits;
}

TEST_CASE("Results match the bit-exact goldens", "[detmath][golden]") {
  const std::vector<std::uint64_t> bits = BitPatterns();
  const testing::GoldenResult result = testing::CheckGolden("detmath", bits);
  INFO(result.message);
  CHECK(result.ok);
}

TEST_CASE("The self-test hash is stable and independent of FMA3",
          "[detmath][determinism]") {
  const std::uint64_t first = dm::SelfTestHash();
  CHECK(dm::SelfTestHash() == first);
  if (fp::SetFma3ForTest(true)) {
    const std::uint64_t with_fma = dm::SelfTestHash();
    CHECK(fp::SetFma3ForTest(false));
    REQUIRE(fp::Fma3State() == 0);
    const std::uint64_t without_fma = dm::SelfTestHash();
    CHECK(with_fma == first);
    CHECK(without_fma == first);
    fp::ConfigureProcess();
    REQUIRE(fp::Fma3State() == 0);
  } else {
    REQUIRE(fp::Fma3State() == -1);
  }
}

TEST_CASE("The FMA3 switch reaches the CRT", "[detmath]") {
  if (!fp::SetFma3ForTest(true)) {
    SUCCEED("no FMA3 switch on this platform");
    return;
  }
  // _get_FMA3_enable reports 1 only on CPUs with FMA3.
  const bool cpu_fma3 = (fp::CpuFeatureBits() & fp::kCpuFma3) != 0;
  CHECK(fp::Fma3State() == (cpu_fma3 ? 1 : 0));
  CHECK(fp::SetFma3ForTest(false));
  REQUIRE(fp::Fma3State() == 0);
}

}  // namespace
}  // namespace se
