// Copyright 2026 The simple-engine-2 Authors.

#include "assets/support/lane_test_support.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "se_assets/background_lane.h"
#include "se_core/thread_role.h"
#include "se_platform/fs.h"

#ifndef SE_TEST_OUT_DIR
#define SE_TEST_OUT_DIR "."
#endif

namespace se::testing {

std::string ScratchDir(std::string_view name) {
  const std::string dir = fs::Join(SE_TEST_OUT_DIR, name);
  if (auto entries = fs::ListDirectory(dir)) {
    for (const std::string& entry : *entries) {
      CHECK(fs::Remove(fs::Join(dir, entry)).has_value());
    }
  }
  CHECK(fs::CreateDirectories(dir).has_value());
  return dir;
}

std::vector<std::byte> PatternBytes(std::size_t size, std::uint32_t seed) {
  std::vector<std::byte> bytes(size);
  std::uint32_t x = seed * 2654435761U + 1;
  for (std::size_t i = 0; i < size; ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    bytes[i] = static_cast<std::byte>(x & 0xFF);
  }
  return bytes;
}

std::vector<std::byte> TextBytes(std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  if (!text.empty()) std::memcpy(bytes.data(), text.data(), text.size());
  return bytes;
}

std::string BytesText(std::span<const std::byte> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool WriteBytes(const std::string& path, std::span<const std::byte> bytes) {
  return fs::WriteFile(path, bytes).has_value();
}

std::vector<std::byte> ReadBytes(const std::string& path) {
  auto bytes = fs::ReadFile(path);
  return bytes ? std::move(*bytes) : std::vector<std::byte>{};
}

std::uint64_t NowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool DrainUntil(BackgroundLane& lane, std::vector<LaneCompletion>& out,
                const std::function<bool()>& done,
                std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    lane.DrainCompletions([&out](LaneCompletion&& completion) {
      out.push_back(std::move(completion));
    });
    if (done()) return true;
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

bool DrainCount(BackgroundLane& lane, std::vector<LaneCompletion>& out,
                std::size_t count, std::chrono::milliseconds timeout) {
  return DrainUntil(
      lane, out, [&out, count] { return out.size() >= count; }, timeout);
}

}  // namespace se::testing
