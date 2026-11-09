// Copyright 2026 The simple-engine-2 Authors.

#include "render_gpu/support/image_compare.h"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>

#include "se_platform/fs.h"
#include "se_render/gpu_resources.h"
#include "se_render/image_io.h"

#ifndef SE_RENDER_GOLDEN_DIR
#error "SE_RENDER_GOLDEN_DIR must name tests/golden/render"
#endif
#ifndef SE_TEST_OUT_DIR
#error "SE_TEST_OUT_DIR must name a writable directory"
#endif

namespace se::testing {
namespace {

// SE_UPDATE_GOLDENS=1 rewrites the goldens (as support/golden.h does for
// the hash goldens; that library carries the null WakeMain, which these
// windowed tests cannot link).
bool UpdatingImageGoldens() {
  const char* value = SDL_getenv("SE_UPDATE_GOLDENS");
  return value != nullptr && std::string_view(value) == "1";
}

}  // namespace

std::string ImageDiff::Describe() const {
  if (!same_size) return "the sizes differ";
  return std::format(
      "{:.3f}% of {} pixels within {}/255 (need {:.0f}%), mean absolute "
      "difference {:.3f}/255 (at most {:.1f}), largest {}",
      100.0 * WithinFraction(), pixels, kPixelTolerance,
      100.0 * kMinWithinFraction, mean_abs_diff, kMaxMeanAbsDiff, max_diff);
}

ImageDiff CompareImages(const TexturePayload& a, const TexturePayload& b) {
  ImageDiff diff;
  diff.same_size = a.width == b.width && a.height == b.height &&
                   a.pixels.size() == a.Bytes() && b.pixels.size() == b.Bytes();
  if (!diff.same_size) return diff;
  diff.pixels = std::uint64_t{a.width} * a.height;
  std::uint64_t total = 0;
  for (std::size_t p = 0; p < diff.pixels; ++p) {
    int worst = 0;
    for (std::size_t c = 0; c < 4; ++c) {
      const int delta =
          std::abs(int{a.pixels[p * 4 + c]} - int{b.pixels[p * 4 + c]});
      worst = std::max(worst, delta);
      total += static_cast<std::uint64_t>(delta);
    }
    if (worst <= kPixelTolerance) ++diff.within;
    diff.max_diff = std::max(diff.max_diff, worst);
  }
  diff.mean_abs_diff =
      static_cast<double>(total) / static_cast<double>(diff.pixels * 4);
  return diff;
}

TexturePayload DiffImage(const TexturePayload& a, const TexturePayload& b) {
  TexturePayload out;
  out.width = std::min(a.width, b.width);
  out.height = std::min(a.height, b.height);
  out.pixels.assign(out.Bytes(), 0);
  for (std::uint32_t y = 0; y < out.height; ++y) {
    for (std::uint32_t x = 0; x < out.width; ++x) {
      const std::size_t pa = (std::size_t{y} * a.width + x) * 4;
      const std::size_t pb = (std::size_t{y} * b.width + x) * 4;
      const std::size_t po = (std::size_t{y} * out.width + x) * 4;
      for (std::size_t c = 0; c < 3; ++c) {
        const int delta =
            std::abs(int{a.pixels[pa + c]} - int{b.pixels[pb + c]});
        out.pixels[po + c] =
            static_cast<std::uint8_t>(std::min(255, delta * 8));
      }
      out.pixels[po + 3] = 255;
    }
  }
  return out;
}

std::string ImageOutDir() { return SE_TEST_OUT_DIR; }

GoldenImageResult CheckGoldenImage(std::string_view driver,
                                   std::string_view name,
                                   const TexturePayload& image) {
  const std::string golden_dir =
      std::string(SE_RENDER_GOLDEN_DIR) + "/" + std::string(driver);
  const std::string golden = golden_dir + "/" + std::string(name) + ".png";
  const std::string out_dir = ImageOutDir() + "/" + std::string(driver);
  if (auto made = fs::CreateDirectories(out_dir); !made) {
    return {false, "cannot create " + out_dir};
  }
  const std::string out = out_dir + "/" + std::string(name) + ".png";
  if (auto written = WritePng(out, image); !written) {
    return {false, "cannot write " + out + ": " + written.error().message};
  }
  if (UpdatingImageGoldens()) {
    if (auto made = fs::CreateDirectories(golden_dir); !made) {
      return {false, "cannot create " + golden_dir};
    }
    if (auto written = WritePng(golden, image); !written) {
      return {false, "cannot write " + golden};
    }
    return {true, "golden rewritten: " + golden};
  }
  auto expected = LoadPng(golden);
  if (!expected) {
    return {false, std::format("no golden {} ({}); run with "
                               "SE_UPDATE_GOLDENS=1 and commit it",
                               golden, expected.error().message)};
  }
  const ImageDiff diff = CompareImages(image, *expected);
  if (diff.Matches()) return {true, diff.Describe()};
  const std::string diff_path = out_dir + "/" + std::string(name) + ".diff.png";
  if (auto written = WritePng(diff_path, DiffImage(image, *expected));
      !written) {
    return {false, "cannot write " + diff_path};
  }
  return {false, std::format("{} differs from {}: {}; see {} and {}", name,
                             golden, diff.Describe(), out, diff_path)};
}

}  // namespace se::testing
