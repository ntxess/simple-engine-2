// Copyright 2026 The simple-engine-2 Authors.

#include "assets/support/png.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace se::testing {
namespace {

void Append(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<std::byte>*>(context);
  const auto* bytes = static_cast<const std::byte*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

std::vector<std::byte> EncodeTestPng(std::uint32_t width, std::uint32_t height,
                                     std::span<const std::uint8_t> rgba) {
  std::vector<std::byte> out;
  if (rgba.size() != std::size_t{width} * height * 4) return out;
  const int ok = stbi_write_png_to_func(
      &Append, &out, static_cast<int>(width), static_cast<int>(height), 4,
      rgba.data(), static_cast<int>(width * 4));
  if (ok == 0) out.clear();
  return out;
}

}  // namespace se::testing
