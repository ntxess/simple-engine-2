// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/guid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "se_core/error.h"

namespace se {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";
// Positions of the dashes in the 36-character form.
constexpr std::array<std::size_t, 4> kDashes = {8, 13, 18, 23};
constexpr std::size_t kGuidTextSize = 36;

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool IsDashPosition(std::size_t i) {
  for (const std::size_t d : kDashes) {
    if (d == i) return true;
  }
  return false;
}

}  // namespace

std::string ToString(const Guid& guid) {
  std::string out;
  out.reserve(kGuidTextSize);
  int nibble = 0;
  for (std::size_t i = 0; i < kGuidTextSize; ++i) {
    if (IsDashPosition(i)) {
      out.push_back('-');
      continue;
    }
    const std::uint64_t word = nibble < 16 ? guid.hi : guid.lo;
    const int shift = 60 - 4 * (nibble % 16);
    out.push_back(kHexDigits[(word >> shift) & 0xF]);
    ++nibble;
  }
  return out;
}

std::expected<Guid, Error> ParseGuid(std::string_view text) {
  if (text.size() != kGuidTextSize) {
    return Fail(ErrorCode::kParse, "a Guid has 36 characters");
  }
  Guid guid;
  int nibble = 0;
  for (std::size_t i = 0; i < kGuidTextSize; ++i) {
    if (IsDashPosition(i)) {
      if (text[i] != '-') {
        return Fail(ErrorCode::kParse, "expected '-' in a Guid");
      }
      continue;
    }
    const int value = HexValue(text[i]);
    if (value < 0) {
      return Fail(ErrorCode::kParse, "invalid hex digit in a Guid");
    }
    std::uint64_t& word = nibble < 16 ? guid.hi : guid.lo;
    word = (word << 4) | static_cast<std::uint64_t>(value);
    ++nibble;
  }
  return guid;
}

}  // namespace se
