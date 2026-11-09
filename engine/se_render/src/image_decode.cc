// Copyright 2026 The simple-engine-2 Authors.
//
// The one translation unit with stb_image's implementation (PNG only, from
// memory; files are read through se::fs).

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

#include "se_core/error.h"
#include "se_platform/fs.h"
#include "se_render/gpu_resources.h"
#include "se_render/image_io.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace se {

std::expected<TexturePayload, Error> DecodePng(std::span<const std::byte> bytes,
                                               std::string_view name) {
  if (bytes.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::unexpected(Error{ErrorCode::kCorrupt, "PNG file too large",
                                 std::string(name), 0, 0});
  }
  int width = 0;
  int height = 0;
  int channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(
      reinterpret_cast<const stbi_uc*>(bytes.data()),
      static_cast<int>(bytes.size()), &width, &height, &channels, 4);
  if (pixels == nullptr) {
    return std::unexpected(
        Error{ErrorCode::kParse,
              std::format("not a PNG stb_image can decode: {}",
                          stbi_failure_reason()),
              std::string(name), 0, 0});
  }
  TexturePayload payload;
  payload.width = static_cast<std::uint32_t>(width);
  payload.height = static_cast<std::uint32_t>(height);
  payload.pixels.resize(payload.Bytes());
  std::memcpy(payload.pixels.data(), pixels, payload.pixels.size());
  stbi_image_free(pixels);
  return payload;
}

std::expected<TexturePayload, Error> LoadPng(std::string_view path) {
  auto bytes = fs::ReadFile(path);
  if (!bytes) {
    Error error = std::move(bytes.error());
    if (error.path.empty()) error.path = std::string(path);
    if (!fs::Exists(path)) error.code = ErrorCode::kNotFound;
    return std::unexpected(std::move(error));
  }
  return DecodePng(*bytes, path);
}

}  // namespace se
