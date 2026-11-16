// Copyright 2026 The simple-engine-2 Authors.
//
// The registered stream types, shared by both backends, and HandlerPathIn.

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

#include "se_core/error.h"
#include "se_crash/crash.h"
#include "src/crash_internal.h"

namespace se::crash {
namespace internal {
namespace {

constexpr std::size_t kTypeCount = kLastStreamType - kFirstStreamType + 1;

struct StreamTable {
  std::mutex mutex;
  std::bitset<kTypeCount> reserved;
};

// Never destroyed: a thread may register its ring while another exits.
StreamTable& Table() {
  static StreamTable* const kTable = new StreamTable();
  return *kTable;
}

std::size_t IndexOf(StreamType type) {
  return static_cast<std::uint32_t>(type) - kFirstStreamType;
}

}  // namespace

std::expected<void, Error> ReserveStream(StreamType type, const void* data,
                                         std::size_t size) {
  const auto value = static_cast<std::uint32_t>(type);
  if (!IsFirstPartyStream(type)) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("dump stream type {:#010x} is outside the "
                            "first-party range {:#010x}-{:#010x}",
                            value, kFirstStreamType, kLastStreamType));
  }
  if (data == nullptr || size == 0) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("dump stream {:#010x} has no memory", value));
  }
  StreamTable& table = Table();
  const std::scoped_lock lock(table.mutex);
  if (table.reserved.test(IndexOf(type))) {
    return Fail(
        ErrorCode::kInvalidArgument,
        std::format("dump stream {:#010x} is already registered", value));
  }
  table.reserved.set(IndexOf(type));
  return {};
}

void ReleaseStream(StreamType type) noexcept {
  if (!IsFirstPartyStream(type)) {
    return;
  }
  StreamTable& table = Table();
  const std::scoped_lock lock(table.mutex);
  table.reserved.reset(IndexOf(type));
}

std::expected<void, Error> UnreserveStream(StreamType type) {
  const auto value = static_cast<std::uint32_t>(type);
  if (!IsFirstPartyStream(type)) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("dump stream type {:#010x} is outside the "
                            "first-party range",
                            value));
  }
  StreamTable& table = Table();
  const std::scoped_lock lock(table.mutex);
  if (!table.reserved.test(IndexOf(type))) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("dump stream {:#010x} is not registered", value));
  }
  table.reserved.reset(IndexOf(type));
  return {};
}

}  // namespace internal

std::string HandlerPathIn(std::string_view dir) {
  std::string path(dir);
  if (!path.empty() && path.back() != '/' && path.back() != '\\') {
    path += '/';
  }
  path += kHandlerFileName;
  return path;
}

}  // namespace se::crash
