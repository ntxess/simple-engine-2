// Copyright 2026 The simple-engine-2 Authors.
//
// The one translation unit with stb_image_write's implementation (PNG
// only). Only the readback test hook, tests and tools call it; an
// executable that does not leaves this object file out of its link.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_platform/fs.h"
#include "se_render/gpu_resources.h"
#include "se_render/image_io.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace se {
namespace {

void AppendBytes(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<std::byte>*>(context);
  const auto* bytes = static_cast<const std::byte*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

std::expected<std::vector<std::byte>, Error> EncodePng(
    std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgba) {
  constexpr auto kMaxSide =
      static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 4);
  if (width == 0 || height == 0 || width > kMaxSide ||
      rgba.size() != std::size_t{width} * height * 4) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("EncodePng: {}x{} does not match {} bytes", width,
                            height, rgba.size()));
  }
  std::vector<std::byte> out;
  const int stride = static_cast<int>(width) * 4;
  if (stbi_write_png_to_func(&AppendBytes, &out, static_cast<int>(width),
                             static_cast<int>(height), 4, rgba.data(),
                             stride) == 0) {
    return Fail(ErrorCode::kIo, "stb_image_write could not encode the PNG");
  }
  return out;
}

std::expected<void, Error> WritePng(std::string_view path, std::uint32_t width,
                                    std::uint32_t height,
                                    std::span<const std::uint8_t> rgba) {
  auto png = EncodePng(width, height, rgba);
  if (!png) return std::unexpected(std::move(png.error()));
  return fs::WriteFile(path, *png);
}

std::expected<void, Error> WritePng(std::string_view path,
                                    const TexturePayload& image) {
  return WritePng(path, image.width, image.height, image.pixels);
}

}  // namespace se
