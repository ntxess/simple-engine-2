// Copyright 2026 The simple-engine-2 Authors.
//
// Image comparison for the renderer's golden images and readbacks
// (docs/milestone-specs.md M7, "Golden images"): two RGBA8 images match
// when they have the same size, at least 98% of their pixels are within
// 8/255 on every channel, and the mean absolute difference over all
// channels is at most 2/255. One condition is added (ADR-065): at least 90%
// of the drawn pixels, those more than 8/255 from the background (the
// expected image's most frequent colour) in either image, are within
// 8/255, so that an image missing its sparse sprites does not pass on its
// background alone. tools/render_shot/compare_images.py applies the same
// rule.
//
// Goldens live in tests/golden/render/<driver>/<name>.png (driver d3d12 or
// vulkan). CheckGoldenImage writes the image and, on a mismatch, a diff
// image into the test output directory; with SE_UPDATE_GOLDENS=1 it
// rewrites the golden instead.

#ifndef TESTS_RENDER_GPU_SUPPORT_IMAGE_COMPARE_H_
#define TESTS_RENDER_GPU_SUPPORT_IMAGE_COMPARE_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "se_render/gpu_resources.h"

namespace se::testing {

inline constexpr int kPixelTolerance = 8;
inline constexpr double kMinWithinFraction = 0.98;
inline constexpr double kMaxMeanAbsDiff = 2.0;
inline constexpr double kMinDrawnWithinFraction = 0.90;

struct ImageDiff {
  bool same_size = false;
  std::uint64_t pixels = 0;
  // Pixels whose channels all differ by at most kPixelTolerance.
  std::uint64_t within = 0;
  // Mean absolute difference over every channel, in 1/255 units.
  double mean_abs_diff = 0.0;
  int max_diff = 0;
  // Pixels more than kPixelTolerance from the background in either image,
  // and how many of them are within kPixelTolerance.
  std::uint64_t drawn = 0;
  std::uint64_t drawn_within = 0;

  [[nodiscard]] double WithinFraction() const noexcept {
    return pixels == 0
               ? 1.0
               : static_cast<double>(within) / static_cast<double>(pixels);
  }
  [[nodiscard]] double DrawnWithinFraction() const noexcept {
    return drawn == 0
               ? 1.0
               : static_cast<double>(drawn_within) / static_cast<double>(drawn);
  }
  [[nodiscard]] bool Matches() const noexcept {
    return same_size && WithinFraction() >= kMinWithinFraction &&
           mean_abs_diff <= kMaxMeanAbsDiff &&
           DrawnWithinFraction() >= kMinDrawnWithinFraction;
  }
  [[nodiscard]] std::string Describe() const;
};

// |b| is the expected image: its most frequent colour is the background.
[[nodiscard]] ImageDiff CompareImages(const TexturePayload& a,
                                      const TexturePayload& b);

// |a| - |b| per channel, absolute and scaled by 8, opaque.
[[nodiscard]] TexturePayload DiffImage(const TexturePayload& a,
                                       const TexturePayload& b);

struct GoldenImageResult {
  bool ok = false;
  std::string message;
};

// Compares |image| with tests/golden/render/<driver>/<name>.png; writes
// <out_dir>/<driver>/<name>.png and, on a mismatch, <name>.diff.png.
[[nodiscard]] GoldenImageResult CheckGoldenImage(std::string_view driver,
                                                 std::string_view name,
                                                 const TexturePayload& image);

// The directory for this test executable's output images.
[[nodiscard]] std::string ImageOutDir();

}  // namespace se::testing

#endif  // TESTS_RENDER_GPU_SUPPORT_IMAGE_COMPARE_H_
