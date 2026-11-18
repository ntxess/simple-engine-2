// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/beve.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"

namespace se::beve {
namespace {

constexpr std::uint8_t kTagString = 2;
constexpr std::uint8_t kTagObject = 3;  // string keys
constexpr std::uint8_t kTypeMask = 0b111;
constexpr std::uint8_t kTypeNumber = 1;
constexpr std::uint8_t kTypeTypedArray = 4;
constexpr std::uint8_t kKindUnsigned = 2;
// Header byte of an unsigned number or typed array element of 2^log2 bytes.
constexpr std::uint8_t UnsignedTag(std::uint8_t type, int log2_bytes) {
  return static_cast<std::uint8_t>(type | (kKindUnsigned << 3) |
                                   (log2_bytes << 5));
}
constexpr std::uint8_t kTagU32 = UnsignedTag(kTypeNumber, 2);
constexpr std::uint8_t kTagU64 = UnsignedTag(kTypeNumber, 3);
constexpr std::uint8_t kTagU8Array = UnsignedTag(kTypeTypedArray, 0);
constexpr std::uint8_t kTagU64Array = UnsignedTag(kTypeTypedArray, 3);
constexpr int kMaxDepth = 16;

class Parser {
 public:
  explicit Parser(std::span<const std::byte> bytes) : bytes_(bytes) {}

  std::expected<Value, Error> ParseValue(int depth) {
    if (depth > kMaxDepth) return Fail(ErrorCode::kCorrupt, "BEVE too deep");
    std::uint8_t tag = 0;
    if (!Byte(&tag)) return Truncated();
    Value value;
    if (tag == kTagString) {
      std::uint64_t size = 0;
      std::span<const std::byte> text;
      if (!Size(&size) || !Take(size, &text)) return Truncated();
      value.kind = Kind::kString;
      value.text = std::string_view(reinterpret_cast<const char*>(text.data()),
                                    text.size());
      return value;
    }
    if ((tag & kTypeMask) == kTypeNumber) {
      if (((tag >> 3) & 0b11) != kKindUnsigned) return Unsupported(tag);
      const int width = 1 << (tag >> 5);
      if (width > 8) return Unsupported(tag);
      std::span<const std::byte> raw;
      if (!Take(static_cast<std::uint64_t>(width), &raw)) return Truncated();
      value.kind = Kind::kUnsigned;
      value.number = Little(raw);
      return value;
    }
    if (tag == kTagObject) {
      std::uint64_t fields = 0;
      if (!Size(&fields)) return Truncated();
      value.kind = Kind::kObject;
      for (std::uint64_t i = 0; i < fields; ++i) {
        std::uint64_t key_size = 0;
        std::span<const std::byte> key;
        if (!Size(&key_size) || !Take(key_size, &key)) return Truncated();
        auto field = ParseValue(depth + 1);
        if (!field) return std::unexpected(field.error());
        value.fields.push_back(
            Field{std::string_view(reinterpret_cast<const char*>(key.data()),
                                   key.size()),
                  std::move(*field)});
      }
      return value;
    }
    if (tag == kTagU8Array || tag == kTagU64Array) {
      const std::uint64_t width = tag == kTagU8Array ? 1 : 8;
      std::uint64_t count = 0;
      if (!Size(&count)) return Truncated();
      if (count > (bytes_.size() - pos_) / width) return Truncated();
      value.kind = tag == kTagU8Array ? Kind::kU8Array : Kind::kU64Array;
      value.count = static_cast<std::size_t>(count);
      if (!Take(count * width, &value.raw)) return Truncated();
      return value;
    }
    return Unsupported(tag);
  }

  [[nodiscard]] bool AtEnd() const { return pos_ == bytes_.size(); }

 private:
  static std::unexpected<Error> Truncated() {
    return Fail(ErrorCode::kCorrupt, "BEVE input is truncated");
  }
  static std::unexpected<Error> Unsupported(std::uint8_t tag) {
    return Fail(ErrorCode::kUnsupported,
                "BEVE header " + std::to_string(tag) + " is not supported");
  }

  static std::uint64_t Little(std::span<const std::byte> raw) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
      value |= static_cast<std::uint64_t>(raw[i]) << (8 * i);
    }
    return value;
  }

  bool Byte(std::uint8_t* out) {
    if (pos_ >= bytes_.size()) return false;
    *out = static_cast<std::uint8_t>(bytes_[pos_++]);
    return true;
  }

  bool Take(std::uint64_t count, std::span<const std::byte>* out) {
    if (count > bytes_.size() - pos_) return false;
    *out = bytes_.subspan(pos_, static_cast<std::size_t>(count));
    pos_ += static_cast<std::size_t>(count);
    return true;
  }

  // A compressed size: the low two bits give the width (1, 2, 4, 8 bytes).
  bool Size(std::uint64_t* out) {
    if (pos_ >= bytes_.size()) return false;
    const int width = 1 << (static_cast<std::uint8_t>(bytes_[pos_]) & 0b11);
    std::span<const std::byte> raw;
    if (!Take(static_cast<std::uint64_t>(width), &raw)) return false;
    *out = Little(raw) >> 2;
    return true;
  }

  std::span<const std::byte> bytes_;
  std::size_t pos_ = 0;
};

}  // namespace

void Writer::Little(std::uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) {
    out_.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
  }
}

void Writer::Size(std::uint64_t size) {
  if (size < (1ULL << 6)) {
    Little(size << 2, 1);
  } else if (size < (1ULL << 14)) {
    Little((size << 2) | 1, 2);
  } else if (size < (1ULL << 30)) {
    Little((size << 2) | 2, 4);
  } else {
    Little((size << 2) | 3, 8);
  }
}

void Writer::BeginObject(std::size_t fields) {
  out_.push_back(static_cast<std::byte>(kTagObject));
  Size(fields);
}

void Writer::Key(std::string_view key) {
  Size(key.size());
  for (const char c : key) out_.push_back(static_cast<std::byte>(c));
}

void Writer::String(std::string_view value) {
  out_.push_back(static_cast<std::byte>(kTagString));
  Key(value);
}

void Writer::U32(std::uint32_t value) {
  out_.push_back(static_cast<std::byte>(kTagU32));
  Little(value, 4);
}

void Writer::U64(std::uint64_t value) {
  out_.push_back(static_cast<std::byte>(kTagU64));
  Little(value, 8);
}

void Writer::Bytes(std::span<const std::byte> bytes) {
  out_.push_back(static_cast<std::byte>(kTagU8Array));
  Size(bytes.size());
  out_.insert(out_.end(), bytes.begin(), bytes.end());
}

void Writer::U64Array(std::span<const std::uint64_t> values) {
  out_.push_back(static_cast<std::byte>(kTagU64Array));
  Size(values.size());
  for (const std::uint64_t v : values) Little(v, 8);
}

const Value* Value::Find(std::string_view key) const noexcept {
  for (const Field& field : fields) {
    if (field.key == key) return &field.value;
  }
  return nullptr;
}

std::vector<std::uint64_t> Value::U64s() const {
  std::vector<std::uint64_t> values;
  if (kind != Kind::kU64Array) return values;
  values.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    std::uint64_t v = 0;
    for (std::size_t b = 0; b < 8; ++b) {
      v |= static_cast<std::uint64_t>(raw[i * 8 + b]) << (8 * b);
    }
    values.push_back(v);
  }
  return values;
}

std::expected<Value, Error> Parse(std::span<const std::byte> bytes) {
  Parser parser(bytes);
  auto value = parser.ParseValue(0);
  if (!value) return value;
  if (!parser.AtEnd()) {
    return Fail(ErrorCode::kCorrupt, "trailing bytes after the BEVE value");
  }
  return value;
}

}  // namespace se::beve
