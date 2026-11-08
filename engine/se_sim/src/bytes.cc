// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/bytes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace se {
namespace {

template <class T>
void PutLittleEndian(std::vector<std::byte>& out, T value) {
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
  }
}

template <class T>
T GetLittleEndian(std::span<const std::byte> bytes) {
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(static_cast<T>(bytes[i]) << (8 * i));
  }
  return value;
}

}  // namespace

void ByteWriter::U8(std::uint8_t value) {
  out_.push_back(static_cast<std::byte>(value));
}

void ByteWriter::U32(std::uint32_t value) { PutLittleEndian(out_, value); }

void ByteWriter::U64(std::uint64_t value) { PutLittleEndian(out_, value); }

void ByteWriter::Bytes(std::span<const std::byte> bytes) {
  out_.insert(out_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::String(std::string_view text) {
  U32(static_cast<std::uint32_t>(text.size()));
  Bytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

void ByteWriter::SizedBytes(std::span<const std::byte> bytes) {
  U32(static_cast<std::uint32_t>(bytes.size()));
  Bytes(bytes);
}

bool ByteReader::U8(std::uint8_t* value) noexcept {
  if (Remaining() < 1) return false;
  *value = static_cast<std::uint8_t>(bytes_[pos_++]);
  return true;
}

bool ByteReader::U32(std::uint32_t* value) noexcept {
  if (Remaining() < 4) return false;
  *value = GetLittleEndian<std::uint32_t>(bytes_.subspan(pos_, 4));
  pos_ += 4;
  return true;
}

bool ByteReader::U64(std::uint64_t* value) noexcept {
  if (Remaining() < 8) return false;
  *value = GetLittleEndian<std::uint64_t>(bytes_.subspan(pos_, 8));
  pos_ += 8;
  return true;
}

bool ByteReader::Bytes(std::size_t count,
                       std::span<const std::byte>* bytes) noexcept {
  if (Remaining() < count) return false;
  *bytes = bytes_.subspan(pos_, count);
  pos_ += count;
  return true;
}

bool ByteReader::String(std::string* text) {
  std::uint32_t size = 0;
  std::span<const std::byte> bytes;
  const std::size_t start = pos_;
  if (!U32(&size) || !Bytes(size, &bytes)) {
    pos_ = start;
    return false;
  }
  text->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

bool ByteReader::SizedBytes(std::vector<std::byte>* bytes) {
  std::uint32_t size = 0;
  std::span<const std::byte> view;
  const std::size_t start = pos_;
  if (!U32(&size) || !Bytes(size, &view)) {
    pos_ = start;
    return false;
  }
  bytes->assign(view.begin(), view.end());
  return true;
}

}  // namespace se
