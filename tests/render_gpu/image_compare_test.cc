// Copyright 2026 The simple-engine-2 Authors.
//
// The image comparison's object, solid-mismatch and fallback conditions
// (ADR-069), on synthetic images and on the committed images: every object
// of each golden, and of the sandbox golden against the committed legacy
// image (tests/golden/render/legacy/stress_600.png, the legacy reference
// render.legacy_image compares with), fails the comparison when it is
// erased or drawn in the fallback magenta. No GPU: label unit.

#include "render_gpu/support/image_compare.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "se_render/gpu_resources.h"
#include "se_render/image_io.h"

namespace se {
namespace {

using Colour = std::array<std::uint8_t, 4>;

void SetPixel(TexturePayload& image, std::size_t p, const Colour& colour) {
  for (std::size_t c = 0; c < 4; ++c) image.pixels[p * 4 + c] = colour[c];
}

Colour PixelAt(const TexturePayload& image, std::size_t p) {
  return {image.pixels[p * 4], image.pixels[p * 4 + 1], image.pixels[p * 4 + 2],
          image.pixels[p * 4 + 3]};
}

TexturePayload Filled(std::uint32_t width, std::uint32_t height,
                      const Colour& colour) {
  TexturePayload image;
  image.width = width;
  image.height = height;
  image.pixels.resize(image.Bytes());
  for (std::size_t p = 0; p < image.pixels.size() / 4; ++p) {
    SetPixel(image, p, colour);
  }
  return image;
}

void FillRect(TexturePayload& image, std::uint32_t x0, std::uint32_t y0,
              std::uint32_t w, std::uint32_t h, const Colour& colour) {
  for (std::uint32_t y = y0; y < y0 + h; ++y) {
    for (std::uint32_t x = x0; x < x0 + w; ++x) {
      SetPixel(image, std::size_t{y} * image.width + x, colour);
    }
  }
}

void Paint(TexturePayload& image, const std::vector<std::size_t>& object,
           const Colour& colour) {
  for (const std::size_t p : object) SetPixel(image, p, colour);
}

constexpr Colour kBlack = {0, 0, 0, 255};
constexpr Colour kWhite = {255, 255, 255, 255};
constexpr Colour kBlue = {40, 60, 200, 255};
constexpr Colour kGrey = {150, 150, 150, 255};

TexturePayload LoadGolden(const std::string& relative) {
  const std::string path = testing::GoldenImageDir() + "/" + relative;
  auto image = LoadPng(path);
  INFO(path);
  REQUIRE(image);
  return std::move(*image);
}

// Every object of |actual| (against |expected|'s background), erased or
// drawn magenta, must make |actual| fail against |expected|; |actual| itself
// must match. |every| > 1 sweeps every |every|-th object only.
void SweepObjects(const TexturePayload& actual, const TexturePayload& expected,
                  const std::string& what, std::size_t every = 1) {
  INFO(what);
  const testing::ImageDiff unchanged = testing::CompareImages(actual, expected);
  INFO(unchanged.Describe());
  REQUIRE(unchanged.Matches());
  const Colour background = testing::BackgroundColour(expected);
  const auto objects = testing::DrawnObjects(actual, background);
  REQUIRE_FALSE(objects.empty());
  std::size_t swept = 0;
  for (std::size_t i = 0; i < objects.size(); i += every) {
    const std::vector<std::size_t>& object = objects[i];
    INFO("object " << i << " of " << objects.size() << ", " << object.size()
                   << " pixels");
    TexturePayload erased = actual;
    Paint(erased, object, background);
    const testing::ImageDiff missing = testing::CompareImages(erased, expected);
    INFO("erased: " << missing.Describe());
    CHECK_FALSE(missing.Matches());
    TexturePayload magenta = actual;
    Paint(magenta, object, testing::kFallbackColour);
    const testing::ImageDiff fallback =
        testing::CompareImages(magenta, expected);
    INFO("magenta: " << fallback.Describe());
    CHECK_FALSE(fallback.Matches());
    ++swept;
  }
  CHECK(swept >= std::min<std::size_t>(objects.size(), 3));
}

TEST_CASE("Image comparison: a sprite missing, added or recoloured fails",
          "[png]") {
  // Two 30 x 30 squares on 200 x 100 black: 1.2% of the pixels drawn.
  TexturePayload expected = Filled(200, 100, kBlack);
  FillRect(expected, 20, 20, 30, 30, kWhite);
  FillRect(expected, 120, 40, 30, 30, kBlue);
  REQUIRE(testing::CompareImages(expected, expected).Matches());
  REQUIRE(testing::CompareImages(expected, expected).objects == 4);

  SECTION("missing") {
    TexturePayload actual = expected;
    FillRect(actual, 120, 40, 30, 30, kBlack);
    const testing::ImageDiff diff = testing::CompareImages(actual, expected);
    INFO(diff.Describe());
    CHECK(diff.worst_object_within == 0.0);
    REQUIRE(diff.worst_object_pixels == 900);
    CHECK_FALSE(diff.Matches());
  }
  SECTION("added") {
    TexturePayload actual = expected;
    FillRect(actual, 80, 5, 6, 6, kWhite);
    const testing::ImageDiff diff = testing::CompareImages(actual, expected);
    INFO(diff.Describe());
    CHECK(diff.worst_object_within == 0.0);
    REQUIRE(diff.worst_object_pixels == 36);
    CHECK_FALSE(diff.Matches());
  }
  SECTION("recoloured") {
    TexturePayload actual = expected;
    FillRect(actual, 120, 40, 30, 30, kGrey);
    CHECK_FALSE(testing::CompareImages(actual, expected).Matches());
  }
  SECTION("shifted by one pixel: thin edges only") {
    TexturePayload actual = Filled(200, 100, kBlack);
    FillRect(actual, 21, 20, 30, 30, kWhite);
    FillRect(actual, 120, 41, 30, 30, kBlue);
    const testing::ImageDiff diff = testing::CompareImages(actual, expected);
    INFO(diff.Describe());
    REQUIRE(diff.solid_mismatches == 0);
    CHECK(diff.Matches());
  }
}

TEST_CASE("Image comparison: overlapping sprites in another order match",
          "[png]") {
  // The legacy Sandbox and the engine order overlapping sprites
  // differently (ADR-065): the overlap differs, nothing goes missing.
  TexturePayload expected = Filled(200, 100, kBlack);
  FillRect(expected, 20, 20, 40, 40, kWhite);
  FillRect(expected, 55, 30, 40, 40, kBlue);
  TexturePayload actual = Filled(200, 100, kBlack);
  FillRect(actual, 55, 30, 40, 40, kBlue);
  FillRect(actual, 20, 20, 40, 40, kWhite);
  const testing::ImageDiff diff = testing::CompareImages(actual, expected);
  INFO(diff.Describe());
  CHECK(diff.drawn_within < diff.drawn);
  REQUIRE(diff.solid_mismatches == 0);
  REQUIRE(diff.fallback_pixels == 0);
  CHECK(diff.Matches());
}

TEST_CASE("Image comparison: a sprite missing from an overlapping group",
          "[png]") {
  // A large square with a small one on its edge, one object: without the
  // small one the object still has most of its pixels within, but the
  // small one leaves a solid block drawn in one image only.
  TexturePayload expected = Filled(200, 100, kBlack);
  FillRect(expected, 20, 20, 50, 50, kWhite);
  FillRect(expected, 66, 30, 10, 10, kBlue);
  TexturePayload actual = Filled(200, 100, kBlack);
  FillRect(actual, 20, 20, 50, 50, kWhite);
  const testing::ImageDiff diff = testing::CompareImages(actual, expected);
  INFO(diff.Describe());
  CHECK(diff.worst_object_within >= testing::kMinObjectWithinFraction);
  CHECK(diff.DrawnWithinFraction() >= testing::kMinDrawnWithinFraction);
  CHECK(diff.solid_mismatches > testing::kMaxSolidMismatches);
  CHECK_FALSE(diff.Matches());
}

TEST_CASE("Image comparison: the fallback magenta inside a group", "[png]") {
  TexturePayload expected = Filled(200, 100, kBlack);
  FillRect(expected, 20, 20, 50, 50, kWhite);
  FillRect(expected, 40, 40, 5, 5, kBlue);
  TexturePayload actual = expected;
  FillRect(actual, 40, 40, 5, 5, testing::kFallbackColour);
  const testing::ImageDiff diff = testing::CompareImages(actual, expected);
  INFO(diff.Describe());
  CHECK(diff.worst_object_within >= testing::kMinObjectWithinFraction);
  REQUIRE(diff.solid_mismatches == 0);
  REQUIRE(diff.fallback_pixels == 25);
  CHECK_FALSE(diff.Matches());
  // Magenta that the expected image has too is not the fallback's.
  CHECK(testing::CompareImages(actual, actual).Matches());
}

TEST_CASE("Every object of each golden, erased or magenta, fails", "[png]") {
  for (const char* driver : {"d3d12", "vulkan"}) {
    for (const char* name : {"sprite_grid", "rotated", "layers_alpha"}) {
      const std::string relative =
          std::string(driver) + "/" + std::string(name) + ".png";
      const TexturePayload golden = LoadGolden(relative);
      SweepObjects(golden, golden, relative);
    }
    // The sandbox golden is 1920 x 1080: every object is swept against the
    // legacy image below; against itself, every eighth.
    const std::string sandbox = std::string(driver) + "/sandbox_600.png";
    const TexturePayload golden = LoadGolden(sandbox);
    SweepObjects(golden, golden, sandbox, 8);
  }
}

TEST_CASE("The sandbox golden matches the legacy image, sprite by sprite",
          "[png]") {
  // render.legacy_image renders the stress scenario at tick 600 as the
  // sandbox golden and compares it with legacy_replay's image, which
  // legacy_image_test.py keeps equal to this committed copy.
  const TexturePayload legacy = LoadGolden("legacy/stress_600.png");
  const TexturePayload engine = LoadGolden("d3d12/sandbox_600.png");
  const testing::ImageDiff diff = testing::CompareImages(engine, legacy);
  INFO(diff.Describe());
  // The draw order differs where ships overlap (ADR-065): not identical.
  CHECK(diff.drawn_within < diff.drawn);
  SweepObjects(engine, legacy, "sandbox_600 against the legacy image");
  // A sprite moved by 40 px fails too.
  const auto objects =
      testing::DrawnObjects(engine, testing::BackgroundColour(legacy));
  REQUIRE_FALSE(objects.empty());
  TexturePayload moved = engine;
  const std::vector<std::size_t>& object = objects.front();
  Paint(moved, object, testing::BackgroundColour(legacy));
  for (const std::size_t p : object) {
    SetPixel(moved, p + 40, PixelAt(engine, p));
  }
  CHECK_FALSE(testing::CompareImages(moved, legacy).Matches());
}

}  // namespace
}  // namespace se
