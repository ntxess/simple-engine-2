// Copyright 2026 The simple-engine-2 Authors.
//
// Cooked atlas pages (docs/milestone-specs.md M9): the .seatlas format
// round trip, corruption checks, the cook from a PNG, and the lane's atlas
// job in its three modes.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "assets/support/lane_test_support.h"
#include "assets/support/png.h"
#include "se_assets/atlas_page.h"
#include "se_assets/background_lane.h"
#include "se_assets/payloads.h"
#include "se_core/thread_role.h"
#include "se_platform/fs.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::DrainCount;
using testing::MainThreadScope;
using testing::ScratchDir;
using testing::WriteBytes;

AtlasPage Gradient(std::uint32_t width, std::uint32_t height) {
  AtlasPage page;
  page.width = width;
  page.height = height;
  page.pixels.resize(page.Bytes());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      std::uint8_t* p = &page.pixels[(std::size_t{y} * width + x) * 4];
      p[0] = static_cast<std::uint8_t>(x);
      p[1] = static_cast<std::uint8_t>(y);
      p[2] = static_cast<std::uint8_t>(x ^ y);
      p[3] = 255;
    }
  }
  page.regions.push_back({"whole", 0, 0, width, height});
  page.regions.push_back({"corner", 1, 2, 3, 4});
  page.source_size = 1234;
  page.source_hash = 0xABCDEF;
  return page;
}

std::expected<AtlasPage, Error> DecodeAll(std::vector<std::byte> file) {
  auto decoder = AtlasPageDecoder::Begin(std::move(file), "memory");
  if (!decoder) return std::unexpected(decoder.error());
  while (!decoder->Done()) {
    if (auto block = decoder->DecodeNextBlock(); !block) {
      return std::unexpected(block.error());
    }
  }
  return decoder->Finish();
}

TEST_CASE("Atlas pages round trip in independent LZ4 blocks", "[atlas]") {
  // 600 x 500 RGBA8 is 1.2 MB: five blocks of 256 KB, the last one short.
  const AtlasPage page = Gradient(600, 500);
  auto encoded = EncodeAtlasPage(page);
  REQUIRE(encoded.has_value());
  CHECK(encoded->size() < page.pixels.size());
  auto decoder = AtlasPageDecoder::Begin(*encoded, "memory");
  REQUIRE(decoder.has_value());
  REQUIRE(decoder->Header().block_count == 5);
  CHECK(decoder->Header().block_bytes == kAtlasBlockBytes);
  CHECK(decoder->Header().regions == page.regions);
  auto decoded = DecodeAll(*encoded);
  REQUIRE(decoded.has_value());
  REQUIRE(decoded->width == 600);
  REQUIRE(decoded->height == 500);
  CHECK(decoded->pixels == page.pixels);
  CHECK(decoded->regions == page.regions);
  REQUIRE(decoded->source_size == 1234);
  REQUIRE(decoded->source_hash == 0xABCDEF);
}

TEST_CASE("A damaged page is an Error, never a wrong image", "[atlas]") {
  const AtlasPage page = Gradient(300, 300);
  auto encoded = EncodeAtlasPage(page, 64 * 1024);
  REQUIRE(encoded.has_value());

  SECTION("not a page") {
    std::vector<std::byte> junk(100, std::byte{7});
    auto decoder = AtlasPageDecoder::Begin(junk, "junk.seatlas");
    REQUIRE_FALSE(decoder.has_value());
    CHECK(decoder.error().code == ErrorCode::kCorrupt);
    REQUIRE(decoder.error().path == "junk.seatlas");
  }
  SECTION("truncated") {
    std::vector<std::byte> cut(
        encoded->begin(),
        encoded->begin() + static_cast<std::ptrdiff_t>(encoded->size() / 2));
    CHECK_FALSE(AtlasPageDecoder::Begin(cut, "cut").has_value());
  }
  SECTION("a flipped byte in a block") {
    std::vector<std::byte> bad = *encoded;
    bad[bad.size() - 10] ^= std::byte{0x5A};
    CHECK_FALSE(DecodeAll(bad).has_value());
  }
  SECTION("another version") {
    std::vector<std::byte> bad = *encoded;
    bad[8] = std::byte{9};
    auto decoder = AtlasPageDecoder::Begin(bad, "v9");
    REQUIRE_FALSE(decoder.has_value());
    CHECK(decoder.error().code == ErrorCode::kVersion);
  }
}

TEST_CASE("The cook decodes a PNG into one region", "[atlas][cook]") {
  const AtlasPage source = Gradient(37, 19);
  const std::vector<std::byte> png =
      testing::EncodeTestPng(source.width, source.height, source.pixels);
  REQUIRE_FALSE(png.empty());
  auto cooked = CookPng(png, "player", "assets/player.png");
  REQUIRE(cooked.has_value());
  CHECK(cooked->pixels == source.pixels);
  REQUIRE(cooked->regions.size() == 1);
  CHECK(cooked->regions[0] == AtlasRegion{"player", 0, 0, 37, 19});
  CHECK(cooked->source_size == png.size());
  CHECK(cooked->source_hash == SourceHash(png));
  std::vector<std::byte> junk(64, std::byte{1});
  auto bad = CookPng(junk, "junk", "junk.png");
  REQUIRE_FALSE(bad.has_value());
  REQUIRE(bad.error().path == "junk.png");
}

TEST_CASE("Cooked pages mirror their sources under the cooked root",
          "[atlas]") {
  REQUIRE(CookedPagePath("out/cooked", "assets/player/player.png") ==
          "out/cooked/assets/player/player.seatlas");
  REQUIRE(CookedPagePath("out/cooked", "assets/bg_city/city 1/1.png") ==
          "out/cooked/assets/bg_city/city 1/1.seatlas");
  REQUIRE(CookedPagePath("c", "a.b/noext") == "c/a.b/noext.seatlas");
  CHECK(CookedPagePath("c", "/abs/x.png").empty());
}

TEST_CASE("The atlas job reads pages, and re-cooks stale ones",
          "[atlas][lane]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("atlas_job");
  const std::string png_path = fs::Join(dir, "tex.png");
  const std::string page_path = fs::Join(dir, "tex.seatlas");
  const AtlasPage first = Gradient(64, 32);
  const std::vector<std::byte> png =
      testing::EncodeTestPng(first.width, first.height, first.pixels);
  REQUIRE(WriteBytes(png_path, png));
  auto cooked = CookPng(png, "tex", png_path);
  REQUIRE(cooked.has_value());
  auto encoded = EncodeAtlasPage(*cooked);
  REQUIRE(encoded.has_value());
  REQUIRE(WriteBytes(page_path, *encoded));

  LaneConfig config;
  config.scan_period_ns = 0;
  auto started = BackgroundLane::Start(config);
  REQUIRE(started.has_value());
  BackgroundLane& lane = **started;
  auto run = [&lane](AtlasRequest request) SE_REQUIRES(g_main) {
    lane.Submit(JobDesc{JobClass::kUrgent}, MakeAtlasJob(std::move(request)));
    std::vector<LaneCompletion> done;
    REQUIRE(DrainCount(lane, done, 1));
    return std::move(done[0].result);
  };
  AtlasRequest request;
  request.name = "tex";
  request.page_path = page_path;
  request.source_path = png_path;

  SECTION("a fresh page loads without a cook") {
    request.mode = AtlasRequest::Mode::kPageIfFresh;
    auto result = run(request);
    REQUIRE(result.has_value());
    const auto& page = std::get<AtlasPage>(*result);
    CHECK_FALSE(page.recooked);
    CHECK(page.pixels == first.pixels);
  }
  SECTION("a changed source makes the page stale: re-cooked") {
    AtlasPage second = Gradient(64, 32);
    second.pixels[0] = 99;
    REQUIRE(
        WriteBytes(png_path, testing::EncodeTestPng(64, 32, second.pixels)));
    request.mode = AtlasRequest::Mode::kPageIfFresh;
    auto result = run(request);
    REQUIRE(result.has_value());
    const auto& page = std::get<AtlasPage>(*result);
    CHECK(page.recooked);
    REQUIRE(page.pixels[0] == 99);
  }
  SECTION("a missing page is cooked from its source") {
    request.page_path = fs::Join(dir, "missing.seatlas");
    request.mode = AtlasRequest::Mode::kPageIfFresh;
    auto result = run(request);
    REQUIRE(result.has_value());
    CHECK(std::get<AtlasPage>(*result).recooked);
  }
  SECTION("Release mode reads the page only") {
    request.page_path = fs::Join(dir, "missing.seatlas");
    request.mode = AtlasRequest::Mode::kPage;
    auto result = run(request);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::kNotFound);
  }
  SECTION("a page without its source loads as it is") {
    request.source_path = fs::Join(dir, "no_source.png");
    request.mode = AtlasRequest::Mode::kPageIfFresh;
    auto result = run(request);
    REQUIRE(result.has_value());
    CHECK_FALSE(std::get<AtlasPage>(*result).recooked);
  }
  SECTION("kCook re-cooks the source") {
    request.mode = AtlasRequest::Mode::kCook;
    auto result = run(request);
    REQUIRE(result.has_value());
    CHECK(std::get<AtlasPage>(*result).recooked);
  }
}

}  // namespace
}  // namespace se
