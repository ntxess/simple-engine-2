// Copyright 2026 The simple-engine-2 Authors.
//
// The texture table parser (se_render/texture_table.h), PNG encoding and
// decoding with stb, and the frame-fault hook's syntax. No GPU.

#include "se_render/texture_table.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "render_gpu/support/image_compare.h"
#include "se_core/error.h"
#include "se_platform/fs.h"
#include "se_render/frame_faults.h"
#include "se_render/gpu_resources.h"
#include "se_render/image_io.h"

namespace se {
namespace {

TEST_CASE("The texture table keeps its entries in file order", "[table]") {
  const auto table = ParseTextureTable(
      R"({ "player": "assets/player/player.png",
           "bg": "assets/bg_city/city 1/1.png",
           "esc\"aped": "a\\b\/c\u00e9" })",
      "texture.json");
  REQUIRE(table);
  REQUIRE(table->size() == 3);
  REQUIRE((*table)[0].name == "player");
  REQUIRE((*table)[0].path == "assets/player/player.png");
  REQUIRE((*table)[1].path == "assets/bg_city/city 1/1.png");
  REQUIRE((*table)[2].name == "esc\"aped");
  REQUIRE((*table)[2].path == "a\\b/c\xC3\xA9");
  const auto empty = ParseTextureTable(" {} ", "texture.json");
  REQUIRE(empty);
  CHECK(empty->empty());
}

TEST_CASE("A malformed texture table is an error with its position",
          "[table]") {
  const auto duplicate =
      ParseTextureTable("{\"a\": \"x\",\n \"a\": \"y\"}", "t.json");
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().code == ErrorCode::kParse);
  REQUIRE(duplicate.error().path == "t.json");
  REQUIRE(duplicate.error().line == 2);
  CHECK(duplicate.error().message.find("twice") != std::string::npos);
  CHECK_FALSE(ParseTextureTable("{\"a\": 3}", "t.json"));
  CHECK_FALSE(ParseTextureTable("[\"a\"]", "t.json"));
  CHECK_FALSE(ParseTextureTable("{\"a\": \"x\"} trailing", "t.json"));
  CHECK_FALSE(ParseTextureTable("{\"a\": \"x\"", "t.json"));
  CHECK_FALSE(ParseTextureTable("{\"a\": \"\\ud800\"}", "t.json"));
}

TEST_CASE("PNGs round-trip through stb", "[png]") {
  TexturePayload image;
  image.width = 3;
  image.height = 2;
  image.pixels = {255, 0, 0, 255, 0,   255, 0,   128, 0,  0,  255, 0,
                  1,   2, 3, 4,   250, 251, 252, 253, 10, 20, 30,  40};
  auto png = EncodePng(image.width, image.height, image.pixels);
  REQUIRE(png);
  auto decoded = DecodePng(*png, "memory");
  REQUIRE(decoded);
  REQUIRE(decoded->width == 3);
  REQUIRE(decoded->height == 2);
  CHECK(decoded->pixels == image.pixels);
  REQUIRE(testing::CompareImages(image, *decoded).max_diff == 0);
}

TEST_CASE("A missing or broken PNG is an error naming the path", "[png]") {
  const std::string missing = testing::ImageOutDir() + "/no_such_file.png";
  auto loaded = LoadPng(missing);
  REQUIRE_FALSE(loaded);
  CHECK(loaded.error().code == ErrorCode::kNotFound);
  CHECK(loaded.error().path == missing);
  const std::vector<std::byte> junk(16, std::byte{0x42});
  auto broken = DecodePng(junk, "junk.png");
  REQUIRE_FALSE(broken);
  REQUIRE(broken.error().path == "junk.png");
}

TEST_CASE("The image comparison applies the golden tolerance", "[png]") {
  TexturePayload a;
  a.width = 10;
  a.height = 10;
  a.pixels.assign(a.Bytes(), 100);
  TexturePayload b = a;
  CHECK(testing::CompareImages(a, b).Matches());
  // Every pixel 8 off: within, mean 8/255 * 3/4 > 2: no match.
  for (std::size_t i = 0; i < b.pixels.size(); i += 4) {
    b.pixels[i] = 108;
    b.pixels[i + 1] = 108;
    b.pixels[i + 2] = 108;
  }
  const testing::ImageDiff shifted = testing::CompareImages(a, b);
  CHECK(shifted.WithinFraction() == 1.0);
  CHECK_FALSE(shifted.Matches());
  // One pixel of 100 far off on every channel: 99% within, mean
  // 155 * 4 / 400 = 1.55: a match. Three such pixels: 97% within.
  b = a;
  for (int c = 0; c < 4; ++c) b.pixels[c] = 255;
  CHECK(testing::CompareImages(a, b).WithinFraction() == 0.99);
  CHECK(testing::CompareImages(a, b).Matches());
  for (int c = 4; c < 12; ++c) b.pixels[c] = 255;
  CHECK_FALSE(testing::CompareImages(a, b).Matches());
  TexturePayload smaller = a;
  smaller.width = 5;
  smaller.pixels.resize(smaller.Bytes());
  CHECK_FALSE(testing::CompareImages(a, smaller).Matches());
}

TEST_CASE("Frame faults parse STEP[:EVERY[:UNTIL]]", "[faults]") {
  const auto plain = ParseFrameFault("copy_pass");
  REQUIRE(plain);
  CHECK(plain->step == FrameStep::kCopyPass);
  REQUIRE(plain->every == 1);
  REQUIRE(plain->until == 0);
  const auto full = ParseFrameFault("swapchain_pass:3:100");
  REQUIRE(full);
  CHECK(full->step == FrameStep::kSwapchainPass);
  REQUIRE(full->every == 3);
  REQUIRE(full->until == 100);
  CHECK_FALSE(ParseFrameFault("render"));
  CHECK_FALSE(ParseFrameFault("build:0"));
  CHECK_FALSE(ParseFrameFault("build:2:x"));
  for (const char* name :
       {"build", "vertices", "upload", "prepare", "copy_pass", "viewport_pass",
        "acquire", "swapchain_pass", "submit"}) {
    const auto step = ParseFrameStep(name);
    REQUIRE(step);
    CHECK(FrameStepName(*step) == name);
  }
}

TEST_CASE("A frame fault fires on its frames only", "[faults]") {
  FrameFaults faults;
  faults.Configure(FrameFaultConfig{FrameStep::kAcquire, 2, 6});
  std::vector<std::uint64_t> fired;
  for (std::uint64_t frame = 1; frame <= 10; ++frame) {
    faults.SetFrame(frame);
    CHECK_FALSE(faults.Fires(FrameStep::kSubmit));
    if (faults.Fires(FrameStep::kAcquire)) fired.push_back(frame);
  }
  CHECK(fired == std::vector<std::uint64_t>{2, 4, 6});
  REQUIRE(faults.Injected(FrameStep::kAcquire) == 3);
  REQUIRE(faults.InjectedTotal() == 3);
  FrameFaults none;
  none.SetFrame(1);
  CHECK_FALSE(none.Fires(FrameStep::kBuild));
}

TEST_CASE("A build-phase fault throws a standard-library exception",
          "[faults]") {
  CHECK_THROWS_AS(ThrowInjectedFault(FrameStep::kBuild), std::out_of_range);
}

}  // namespace
}  // namespace se
