// Copyright 2026 The simple-engine-2 Authors.
//
// PNG encoding for the asset tests (stb_image_write, static in png.cc).

#ifndef TESTS_ASSETS_SUPPORT_PNG_H_
#define TESTS_ASSETS_SUPPORT_PNG_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace se::testing {

// RGBA8 rows top to bottom; empty on failure.
[[nodiscard]] std::vector<std::byte> EncodeTestPng(
    std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgba);

}  // namespace se::testing

#endif  // TESTS_ASSETS_SUPPORT_PNG_H_
