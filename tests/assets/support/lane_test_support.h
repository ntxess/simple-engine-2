// Copyright 2026 The simple-engine-2 Authors.
//
// Helpers for the background lane's tests: a scratch directory per test,
// file bytes, waiting for completions on main without blocking forever.

#ifndef TESTS_ASSETS_SUPPORT_LANE_TEST_SUPPORT_H_
#define TESTS_ASSETS_SUPPORT_LANE_TEST_SUPPORT_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "se_assets/background_lane.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se::testing {

// A fresh, empty directory under the build tree's assets_out, named after
// |name|.
[[nodiscard]] std::string ScratchDir(std::string_view name);

[[nodiscard]] std::vector<std::byte> PatternBytes(std::size_t size,
                                                  std::uint32_t seed);
[[nodiscard]] std::vector<std::byte> TextBytes(std::string_view text);
[[nodiscard]] std::string BytesText(std::span<const std::byte> bytes);
bool WriteBytes(const std::string& path, std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> ReadBytes(const std::string& path);

[[nodiscard]] std::uint64_t NowNs() noexcept;

// Drains |lane| into |out| until |done| returns true or |timeout| passes;
// true when |done| did. Sleeps 1 ms between drains.
bool DrainUntil(BackgroundLane& lane, std::vector<LaneCompletion>& out,
                const std::function<bool()>& done,
                std::chrono::milliseconds timeout = std::chrono::seconds(20))
    SE_REQUIRES(g_main);

// Drains until |count| completions arrived.
bool DrainCount(BackgroundLane& lane, std::vector<LaneCompletion>& out,
                std::size_t count,
                std::chrono::milliseconds timeout = std::chrono::seconds(20))
    SE_REQUIRES(g_main);

}  // namespace se::testing

#endif  // TESTS_ASSETS_SUPPORT_LANE_TEST_SUPPORT_H_
