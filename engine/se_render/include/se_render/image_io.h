// Copyright 2026 The simple-engine-2 Authors.
//
// PNG decoding and writing with stb (docs/milestone-specs.md M7; ADR-060).
// Decoding uses stb_image (the PNG loader only), the same decoder SFML
// uses, so the engine samples the legacy tree's texels. Since M9 the
// runtime reads cooked pages through the background lane instead (stb
// runs at import: se_assets' cook); this decoder serves tests and tools.
// Writing uses stb_image_write and serves the readback test hook, tests and
// tools (render_shot, goldens); each implementation lives in one translation
// unit of se_render.

#ifndef SE_RENDER_IMAGE_IO_H_
#define SE_RENDER_IMAGE_IO_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "se_core/error.h"
#include "se_render/gpu_resources.h"

namespace se {

// Decodes a PNG to RGBA8 (straight alpha). |name| names the source in
// errors.
[[nodiscard]] std::expected<TexturePayload, Error> DecodePng(
    std::span<const std::byte> bytes, std::string_view name);

// Reads and decodes |path| (se::fs, so SDL's file API). A missing file is
// an Error naming the path (kNotFound).
[[nodiscard]] std::expected<TexturePayload, Error> LoadPng(
    std::string_view path);

// Encodes RGBA8 pixels (rows top to bottom) as a PNG.
[[nodiscard]] std::expected<std::vector<std::byte>, Error> EncodePng(
    std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgba);

// EncodePng, then se::fs::WriteFile.
[[nodiscard]] std::expected<void, Error> WritePng(
    std::string_view path, std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgba);
[[nodiscard]] std::expected<void, Error> WritePng(std::string_view path,
                                                  const TexturePayload& image);

}  // namespace se

#endif  // SE_RENDER_IMAGE_IO_H_
