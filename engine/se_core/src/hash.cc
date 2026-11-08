// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/hash.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace se {

void Hasher::AddBytes(std::span<const std::byte> bytes) noexcept {
  std::uint64_t word = 0;
  int filled = 0;
  for (const std::byte b : bytes) {
    word |= static_cast<std::uint64_t>(b) << (8 * filled);
    if (++filled == 8) {
      AddU64(word);
      word = 0;
      filled = 0;
    }
  }
  if (filled != 0) {
    AddU64(word);
  }
  AddU64(bytes.size());
}

void Hasher::AddString(std::string_view text) noexcept {
  AddBytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

}  // namespace se
