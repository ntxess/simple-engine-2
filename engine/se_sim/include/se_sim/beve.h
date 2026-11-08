// Copyright 2026 The simple-engine-2 Authors.
//
// A small first-party writer and reader for the subset of BEVE (glaze's
// Binary Efficient Versatile Encoding, https://github.com/beve-org/beve)
// that replay files use: objects with string keys, strings, unsigned
// integers, and typed arrays of u8 and u64. The output is ordinary BEVE that
// glaze reads (tests/kernel/replay_test.cc converts it with
// glz::beve_to_json). It exists so replays need no glaze outside the
// component-registry units (ADR-010).
//
// Encoding: one header byte per value, bits 0-2 the type (2 string,
// 1 number, 3 object, 4 typed array), bits 3-4 the number kind (2 unsigned)
// or the key kind (0 string), bits 5-7 log2 of the byte width. Sizes are
// compressed integers: the low two bits select 1, 2, 4 or 8 bytes holding
// size << 2. Numbers are little-endian.

#ifndef SE_SIM_BEVE_H_
#define SE_SIM_BEVE_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"

namespace se::beve {

class Writer {
 public:
  // An object with |fields| key-value pairs follows.
  void BeginObject(std::size_t fields);
  void Key(std::string_view key);
  void String(std::string_view value);
  void U32(std::uint32_t value);
  void U64(std::uint64_t value);
  // Typed arrays.
  void Bytes(std::span<const std::byte> bytes);
  void U64Array(std::span<const std::uint64_t> values);

  [[nodiscard]] std::vector<std::byte> Take() noexcept {
    return std::move(out_);
  }

 private:
  void Size(std::uint64_t size);
  void Little(std::uint64_t value, int bytes);

  std::vector<std::byte> out_;
};

enum class Kind : std::uint8_t {
  kString,
  kUnsigned,
  kObject,
  kU8Array,
  kU64Array,
};

struct Field;

// A parsed value. Strings and arrays point into the parsed bytes.
struct Value {
  Kind kind = Kind::kUnsigned;
  std::uint64_t number = 0;
  std::string_view text;
  // Array payload, little-endian elements.
  std::span<const std::byte> raw;
  std::size_t count = 0;
  std::vector<Field> fields;

  // The field |key| of an object, or nullptr.
  [[nodiscard]] const Value* Find(std::string_view key) const noexcept;
  // The elements of a u64 array.
  [[nodiscard]] std::vector<std::uint64_t> U64s() const;
};

struct Field {
  std::string_view key;
  Value value;
};

// Parses one value covering all of |bytes|.
[[nodiscard]] std::expected<Value, Error> Parse(
    std::span<const std::byte> bytes);

}  // namespace se::beve

#endif  // SE_SIM_BEVE_H_
