// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/dump_stream.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace se {

std::string_view LogRecord::File() const noexcept {
  std::size_t size = 0;
  while (size < kLogRecordFileSize && file[size] != '\0') ++size;
  return {file, size};
}

void StoreDumpName(std::span<char> field, std::string_view name) noexcept {
  for (std::size_t i = 0; i < field.size(); ++i) {
    const char c = i < name.size() ? name[i] : '\0';
    std::atomic_ref<char>(field[i]).store(c, std::memory_order_relaxed);
  }
}

void InitDumpStreamHeader(DumpStreamHeader& header, DumpStreamKind kind,
                          std::uint32_t record_size, std::uint32_t capacity,
                          std::string_view name) noexcept {
  header.magic = kDumpStreamMagic;
  header.version = kDumpStreamVersion;
  header.kind = static_cast<std::uint16_t>(kind);
  header.record_size = record_size;
  header.capacity = capacity;
  header.head = 0;
  header.dropped = 0;
  header.os_thread_id = 0;
  header.role = 0;
  StoreDumpName(header.name, name);
}

}  // namespace se
