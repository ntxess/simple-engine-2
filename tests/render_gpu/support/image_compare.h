// Copyright 2026 The simple-engine-2 Authors.
//
// Image comparison for the renderer's golden images and readbacks
// (docs/milestone-specs.md M7, "Golden images"): two RGBA8 images match
// when they have the same size, at least 98% of their pixels are within
// 8/255 on every channel, and the mean absolute difference over all
// channels is at most 2/255. Conditions are added so that a missing sprite
// or one drawn with the fallback texture cannot pass on the rest of the
// image. A drawn pixel is one more than 8/255 from the background (the
// expected image's most frequent colour); an object is an 8-connected
// group of one image's drawn pixels.
//   - At least 90% of the pixels drawn in either image are within 8/255
//     (ADR-065).
//   - Every object of either image of at least 8 pixels has at least 25% of
//     its pixels within 8/255: a sprite that is missing, added, or drawn in
//     other colours fails on its own; smaller specks are left to the
//     image-wide conditions (ADR-069).
//   - At most 8 solid mismatches: pixels that, with all 8 neighbours, are
//     drawn in one image and background in the other. A sprite missing
//     from a group of overlapping ones leaves such a block; rasterization
//     and draw-order differences leave only thin edges (ADR-069).
//   - At most 8 pixels show the fallback magenta (GpuResources' texture for
//     a missing or stale one) in the actual image where the expected image
//     does not (ADR-069).
// tools/render_shot/compare_images.py applies the same rule.
//
// Goldens live in tests/golden/render/<driver>/<name>.png (driver d3d12 or
// vulkan). CheckGoldenImage writes the image and, on a mismatch, a diff
// image into the test output directory; with SE_UPDATE_GOLDENS=1 it
// rewrites the golden instead.

#ifndef TESTS_RENDER_GPU_SUPPORT_IMAGE_COMPARE_H_
#define TESTS_RENDER_GPU_SUPPORT_IMAGE_COMPARE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "se_render/gpu_resources.h"

namespace se::testing {

inline constexpr int kPixelTolerance = 8;
inline constexpr double kMinWithinFraction = 0.98;
inline constexpr double kMaxMeanAbsDiff = 2.0;
inline constexpr double kMinDrawnWithinFraction = 0.90;
inline constexpr double kMinObjectWithinFraction = 0.25;
inline constexpr std::uint64_t kMinObjectPixels = 8;
inline constexpr std::uint64_t kMaxSolidMismatches = 8;
inline constexpr std::uint64_t kMaxFallbackPixels = 8;
// GpuResources' fallback texture (a 1x1 opaque magenta).
inline constexpr std::array<std::uint8_t, 4> kFallbackColour = {255, 0, 255,
                                                                255};

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
  // The objects of both images with at least kMinObjectPixels pixels, and
  // the one with the smallest fraction of its pixels within
  // kPixelTolerance (1 and 0 pixels without objects).
  std::uint64_t objects = 0;
  double worst_object_within = 1.0;
  std::uint64_t worst_object_pixels = 0;
  // Pixels drawn in exactly one image whose 8 neighbours are too.
  std::uint64_t solid_mismatches = 0;
  // Pixels within kPixelTolerance of kFallbackColour in the actual image
  // and not in the expected one.
  std::uint64_t fallback_pixels = 0;

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
           DrawnWithinFraction() >= kMinDrawnWithinFraction &&
           worst_object_within >= kMinObjectWithinFraction &&
           solid_mismatches <= kMaxSolidMismatches &&
           fallback_pixels <= kMaxFallbackPixels;
  }
  [[nodiscard]] std::string Describe() const;
};

// |a| is the actual image, |b| the expected one: its most frequent colour
// is the background.
[[nodiscard]] ImageDiff CompareImages(const TexturePayload& a,
                                      const TexturePayload& b);

// The most frequent colour of |image| (the first one on a tie).
[[nodiscard]] std::array<std::uint8_t, 4> BackgroundColour(
    const TexturePayload& image);

// The objects of |image| with |background| as the background: each a list
// of pixel indices, in the scan order of their first pixel.
[[nodiscard]] std::vector<std::vector<std::size_t>> DrawnObjects(
    const TexturePayload& image, const std::array<std::uint8_t, 4>& background);

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

// tests/golden/render.
[[nodiscard]] std::string GoldenImageDir();

// The directory for this test executable's output images.
[[nodiscard]] std::string ImageOutDir();

}  // namespace se::testing

#endif  // TESTS_RENDER_GPU_SUPPORT_IMAGE_COMPARE_H_
