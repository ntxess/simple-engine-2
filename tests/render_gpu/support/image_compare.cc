// Copyright 2026 The simple-engine-2 Authors.

#include "render_gpu/support/image_compare.h"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

std::uint32_t PixelKey(const std::uint8_t* pixel) {
  return std::uint32_t{pixel[0]} | (std::uint32_t{pixel[1]} << 8) |
         (std::uint32_t{pixel[2]} << 16) | (std::uint32_t{pixel[3]} << 24);
}

int WorstChannel(const std::uint8_t* pixel,
                 const std::array<std::uint8_t, 4>& colour) {
  int worst = 0;
  for (std::size_t c = 0; c < 4; ++c) {
    worst = std::max(worst, std::abs(int{pixel[c]} - int{colour[c]}));
  }
  return worst;
}

// Per-pixel flags of a comparison.
constexpr std::uint8_t kDrawnA = 1U;
constexpr std::uint8_t kDrawnB = 2U;
constexpr std::uint8_t kWithin = 4U;
constexpr std::uint8_t kSeenA = 8U;
constexpr std::uint8_t kSeenB = 16U;

bool Has(std::uint8_t flags, std::uint8_t which) {
  return (flags & which) != 0;
}

// Drawn in exactly one of the two images.
bool PresenceDiffers(std::uint8_t flags) {
  return Has(flags, kDrawnA) != Has(flags, kDrawnB);
}

// Calls |visit| with each 8-connected group of the pixels whose |mask| flag
// is set, marking them with |seen|; groups come in the scan order of their
// first pixel.
template <typename Visit>
void ForEachObject(std::vector<std::uint8_t>& flags, std::uint32_t width,
                   std::uint32_t height, std::uint8_t mask, std::uint8_t seen,
                   const Visit& visit) {
  std::vector<std::size_t> stack;
  std::vector<std::size_t> object;
  const auto w = static_cast<std::int64_t>(width);
  const auto h = static_cast<std::int64_t>(height);
  for (std::size_t start = 0; start < flags.size(); ++start) {
    if (!Has(flags[start], mask) || Has(flags[start], seen)) continue;
    flags[start] |= seen;
    stack.assign(1, start);
    object.clear();
    while (!stack.empty()) {
      const std::size_t p = stack.back();
      stack.pop_back();
      object.push_back(p);
      const auto x = static_cast<std::int64_t>(p % width);
      const auto y = static_cast<std::int64_t>(p / width);
      for (std::int64_t ny = std::max<std::int64_t>(0, y - 1);
           ny <= std::min(h - 1, y + 1); ++ny) {
        for (std::int64_t nx = std::max<std::int64_t>(0, x - 1);
             nx <= std::min(w - 1, x + 1); ++nx) {
          const auto q = static_cast<std::size_t>(ny * w + nx);
          if (Has(flags[q], mask) && !Has(flags[q], seen)) {
            flags[q] |= seen;
            stack.push_back(q);
          }
        }
      }
    }
    visit(object);
  }
}

}  // namespace

std::array<std::uint8_t, 4> BackgroundColour(const TexturePayload& image) {
  // Counted in runs: rendered images are mostly long runs of the clear
  // colour.
  std::unordered_map<std::uint32_t, std::uint64_t> counts;
  std::uint32_t best = 0;
  std::uint64_t best_count = 0;
  const std::size_t pixels = image.pixels.size() / 4;
  std::size_t p = 0;
  while (p < pixels) {
    const std::uint32_t key = PixelKey(&image.pixels[p * 4]);
    std::size_t run = 1;
    while (p + run < pixels && PixelKey(&image.pixels[(p + run) * 4]) == key) {
      ++run;
    }
    std::uint64_t& count = counts[key];
    count += run;
    if (count > best_count) {
      best = key;
      best_count = count;
    }
    p += run;
  }
  return {static_cast<std::uint8_t>(best & 0xFFU),
          static_cast<std::uint8_t>((best >> 8) & 0xFFU),
          static_cast<std::uint8_t>((best >> 16) & 0xFFU),
          static_cast<std::uint8_t>((best >> 24) & 0xFFU)};
}

std::vector<std::vector<std::size_t>> DrawnObjects(
    const TexturePayload& image,
    const std::array<std::uint8_t, 4>& background) {
  std::vector<std::vector<std::size_t>> objects;
  if (image.pixels.size() != image.Bytes()) return objects;
  std::vector<std::uint8_t> flags(image.pixels.size() / 4, 0);
  for (std::size_t p = 0; p < flags.size(); ++p) {
    if (WorstChannel(&image.pixels[p * 4], background) > kPixelTolerance) {
      flags[p] = kDrawnA;
    }
  }
  ForEachObject(flags, image.width, image.height, kDrawnA, kSeenA,
                [&](const std::vector<std::size_t>& object) {
                  objects.push_back(object);
                });
  return objects;
}

std::string ImageDiff::Describe() const {
  if (!same_size) return "the sizes differ";
  return std::format(
      "{:.3f}% of {} pixels within {}/255 (need {:.0f}%), mean absolute "
      "difference {:.3f}/255 (at most {:.1f}), largest {}; {:.3f}% of {} "
      "drawn pixels within (need {:.0f}%); worst of {} objects {:.1f}% of {} "
      "pixels within (need {:.0f}%); {} solid mismatches (at most {}); {} "
      "fallback-magenta pixels (at most {})",
      100.0 * WithinFraction(), pixels, kPixelTolerance,
      100.0 * kMinWithinFraction, mean_abs_diff, kMaxMeanAbsDiff, max_diff,
      100.0 * DrawnWithinFraction(), drawn, 100.0 * kMinDrawnWithinFraction,
      objects, 100.0 * worst_object_within, worst_object_pixels,
      100.0 * kMinObjectWithinFraction, solid_mismatches, kMaxSolidMismatches,
      fallback_pixels, kMaxFallbackPixels);
}

ImageDiff CompareImages(const TexturePayload& a, const TexturePayload& b) {
  ImageDiff diff;
  diff.same_size = a.width == b.width && a.height == b.height &&
                   a.pixels.size() == a.Bytes() && b.pixels.size() == b.Bytes();
  if (!diff.same_size) return diff;
  diff.pixels = std::uint64_t{a.width} * a.height;
  const std::array<std::uint8_t, 4> background = BackgroundColour(b);
  std::vector<std::uint8_t> flags(static_cast<std::size_t>(diff.pixels), 0);
  std::uint64_t total = 0;
  for (std::size_t p = 0; p < flags.size(); ++p) {
    const std::uint8_t* pa = &a.pixels[p * 4];
    const std::uint8_t* pb = &b.pixels[p * 4];
    int worst = 0;
    for (std::size_t c = 0; c < 4; ++c) {
      const int delta = std::abs(int{pa[c]} - int{pb[c]});
      worst = std::max(worst, delta);
      total += static_cast<std::uint64_t>(delta);
    }
    std::uint8_t flag = 0;
    if (worst <= kPixelTolerance) {
      flag |= kWithin;
      ++diff.within;
    }
    diff.max_diff = std::max(diff.max_diff, worst);
    if (WorstChannel(pa, background) > kPixelTolerance) flag |= kDrawnA;
    if (WorstChannel(pb, background) > kPixelTolerance) flag |= kDrawnB;
    if (Has(flag, kDrawnA | kDrawnB)) {
      ++diff.drawn;
      if (Has(flag, kWithin)) ++diff.drawn_within;
    }
    if (WorstChannel(pa, kFallbackColour) <= kPixelTolerance &&
        WorstChannel(pb, kFallbackColour) > kPixelTolerance) {
      ++diff.fallback_pixels;
    }
    flags[p] = flag;
  }
  diff.mean_abs_diff =
      static_cast<double>(total) / static_cast<double>(diff.pixels * 4);

  // Each object of either image, by the fraction of its pixels within.
  const auto measure = [&](const std::vector<std::size_t>& object) {
    if (object.size() < kMinObjectPixels) return;
    std::uint64_t object_within = 0;
    for (const std::size_t p : object) {
      if (Has(flags[p], kWithin)) ++object_within;
    }
    const double fraction =
        static_cast<double>(object_within) / static_cast<double>(object.size());
    ++diff.objects;
    if (fraction < diff.worst_object_within) {
      diff.worst_object_within = fraction;
      diff.worst_object_pixels = object.size();
    }
  };
  ForEachObject(flags, a.width, a.height, kDrawnA, kSeenA, measure);
  ForEachObject(flags, a.width, a.height, kDrawnB, kSeenB, measure);

  // Solid mismatches: drawn in one image only, and so are all 8 neighbours.
  const std::size_t w = a.width;
  for (std::size_t y = 1; y + 1 < a.height; ++y) {
    for (std::size_t x = 1; x + 1 < w; ++x) {
      if (!PresenceDiffers(flags[y * w + x])) continue;
      bool solid = true;
      for (std::size_t ny = y - 1; ny <= y + 1 && solid; ++ny) {
        for (std::size_t nx = x - 1; nx <= x + 1; ++nx) {
          if (!PresenceDiffers(flags[ny * w + nx])) {
            solid = false;
            break;
          }
        }
      }
      if (solid) ++diff.solid_mismatches;
    }
  }
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

std::string GoldenImageDir() { return SE_RENDER_GOLDEN_DIR; }

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
