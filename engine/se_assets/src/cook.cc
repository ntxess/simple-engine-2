// Copyright 2026 The simple-engine-2 Authors.
//
// The import side of atlas pages: stb_image decodes a PNG (the PNG loader
// only, as se_render's does, so the texels match the legacy tree's SFML
// decode). This translation unit holds se_assets' copy of stb_image's
// implementation, static (STB_IMAGE_STATIC), so it never collides with
// se_render's. It runs only at import: in tools/asset_cook, and on the
// background lane in dev builds when a page is missing or stale, or its
// source changed.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "se_assets/atlas_page.h"
#include "se_assets/payloads.h"
#include "se_core/error.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace se {

std::expected<AtlasPage, Error> CookPng(std::span<const std::byte> png,
                                        std::string_view name,
                                        std::string_view source_path) {
  if (png.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::unexpected(Error{ErrorCode::kCorrupt, "PNG file too large",
                                 std::string(source_path), 0, 0});
  }
  int width = 0;
  int height = 0;
  int channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(
      reinterpret_cast<const stbi_uc*>(png.data()),
      static_cast<int>(png.size()), &width, &height, &channels, 4);
  if (pixels == nullptr) {
    return std::unexpected(
        Error{ErrorCode::kParse,
              std::format("not a PNG stb_image can decode: {}",
                          stbi_failure_reason()),
              std::string(source_path), 0, 0});
  }
  AtlasPage page;
  page.width = static_cast<std::uint32_t>(width);
  page.height = static_cast<std::uint32_t>(height);
  page.pixels.resize(page.Bytes());
  std::memcpy(page.pixels.data(), pixels, page.pixels.size());
  stbi_image_free(pixels);
  page.regions.push_back(
      AtlasRegion{std::string(name), 0, 0, page.width, page.height});
  page.source_size = png.size();
  page.source_hash = SourceHash(png);
  return page;
}

}  // namespace se
