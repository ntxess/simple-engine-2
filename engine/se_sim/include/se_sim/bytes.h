// Copyright 2026 The simple-engine-2 Authors.
//
// Little-endian byte streams for the SessionLog and replay encodings. The
// byte order is fixed, so files are the same on every platform.

#ifndef SE_SIM_BYTES_H_
#define SE_SIM_BYTES_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace se {

class ByteWriter {
 public:
  void U8(std::uint8_t value);
  void U32(std::uint32_t value);
  void U64(std::uint64_t value);
  void Bytes(std::span<const std::byte> bytes);
  // A u32 length, then the bytes.
  void String(std::string_view text);
  void SizedBytes(std::span<const std::byte> bytes);

  [[nodiscard]] const std::vector<std::byte>& Data() const noexcept {
    return out_;
  }
  [[nodiscard]] std::vector<std::byte> Take() noexcept {
    return std::move(out_);
  }

 private:
  std::vector<std::byte> out_;
};

// Reads what ByteWriter wrote. Each call returns false, and reads nothing,
// when the input is too short.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> bytes) noexcept
      : bytes_(bytes) {}

  bool U8(std::uint8_t* value) noexcept;
  bool U32(std::uint32_t* value) noexcept;
  bool U64(std::uint64_t* value) noexcept;
  bool Bytes(std::size_t count, std::span<const std::byte>* bytes) noexcept;
  bool String(std::string* text);
  bool SizedBytes(std::vector<std::byte>* bytes);

  [[nodiscard]] bool AtEnd() const noexcept { return pos_ == bytes_.size(); }
  [[nodiscard]] std::size_t Remaining() const noexcept {
    return bytes_.size() - pos_;
  }

 private:
  std::span<const std::byte> bytes_;
  std::size_t pos_ = 0;
};

}  // namespace se

#endif  // SE_SIM_BYTES_H_
