// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/fs_faults.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace se::fs {
namespace {

constexpr std::uint64_t kNsPerMs = 1'000'000;

std::atomic<std::uint32_t> g_write_delay_ms{0};
std::atomic<std::uint32_t> g_read_delay_ms{0};
std::atomic<std::uint32_t> g_rename_busy_ms{0};
// When the first refused replace happened (SDL_GetTicksNS, 0: not yet).
std::atomic<std::uint64_t> g_busy_since_ns{0};
std::atomic<std::uint64_t> g_writes_delayed{0};
std::atomic<std::uint64_t> g_reads_delayed{0};
std::atomic<std::uint64_t> g_renames_refused{0};
// FsFaults::rename_busy_suffix, cut to its first kSuffixCapacity bytes:
// atomics, copied without allocating or locking, so SetFsFaults and
// RefuseReplace stay noexcept. A test sets it before the renames it is for.
constexpr std::size_t kSuffixCapacity = 256;
std::array<std::atomic<char>, kSuffixCapacity> g_busy_suffix{};
std::atomic<std::size_t> g_busy_suffix_size{0};

// Copies the suffix into |out|; returns its length.
std::size_t LoadSuffix(std::array<char, kSuffixCapacity>& out) noexcept {
  const std::size_t size = g_busy_suffix_size.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < size; ++i) {
    out[i] = g_busy_suffix[i].load(std::memory_order_relaxed);
  }
  return size;
}

}  // namespace

void SetFsFaults(const FsFaults& faults) noexcept {
  if constexpr (!kFsFaultHooks) return;
  const std::string_view suffix = faults.rename_busy_suffix;
  const std::size_t size = std::min(suffix.size(), kSuffixCapacity);
  for (std::size_t i = 0; i < size; ++i) {
    g_busy_suffix[i].store(suffix[i], std::memory_order_relaxed);
  }
  g_busy_suffix_size.store(size, std::memory_order_release);
  g_busy_since_ns.store(0, std::memory_order_relaxed);
  g_write_delay_ms.store(faults.write_step_delay_ms, std::memory_order_relaxed);
  g_read_delay_ms.store(faults.read_step_delay_ms, std::memory_order_relaxed);
  g_rename_busy_ms.store(faults.rename_busy_ms, std::memory_order_release);
}

void ClearFsFaults() noexcept { SetFsFaults(FsFaults{}); }

FsFaults GetFsFaults() {
  FsFaults faults;
  faults.write_step_delay_ms = g_write_delay_ms.load(std::memory_order_relaxed);
  faults.read_step_delay_ms = g_read_delay_ms.load(std::memory_order_relaxed);
  faults.rename_busy_ms = g_rename_busy_ms.load(std::memory_order_acquire);
  std::array<char, kSuffixCapacity> suffix{};
  faults.rename_busy_suffix.assign(suffix.data(), LoadSuffix(suffix));
  return faults;
}

FsFaultStats GetFsFaultStats() noexcept {
  FsFaultStats stats;
  stats.writes_delayed = g_writes_delayed.load(std::memory_order_relaxed);
  stats.reads_delayed = g_reads_delayed.load(std::memory_order_relaxed);
  stats.renames_refused = g_renames_refused.load(std::memory_order_relaxed);
  return stats;
}

namespace internal {

void BeforeWriteStep() noexcept {
  if constexpr (!kFsFaultHooks) return;
  const std::uint32_t delay = g_write_delay_ms.load(std::memory_order_relaxed);
  if (delay == 0) return;
  g_writes_delayed.fetch_add(1, std::memory_order_relaxed);
  SDL_DelayNS(std::uint64_t{delay} * kNsPerMs);
}

void BeforeReadStep() noexcept {
  if constexpr (!kFsFaultHooks) return;
  const std::uint32_t delay = g_read_delay_ms.load(std::memory_order_relaxed);
  if (delay == 0) return;
  g_reads_delayed.fetch_add(1, std::memory_order_relaxed);
  SDL_DelayNS(std::uint64_t{delay} * kNsPerMs);
}

bool RefuseReplace(std::string_view destination) noexcept {
  if constexpr (!kFsFaultHooks) return false;
  const std::uint32_t busy = g_rename_busy_ms.load(std::memory_order_acquire);
  if (busy == 0) return false;
  std::array<char, kSuffixCapacity> suffix{};
  const std::string_view only(suffix.data(), LoadSuffix(suffix));
  if (!only.empty() && !destination.ends_with(only)) return false;
  const std::uint64_t now = SDL_GetTicksNS();
  std::uint64_t since = g_busy_since_ns.load(std::memory_order_relaxed);
  if (since == 0) {
    // The first refusal opens the window; a racing first caller's stamp
    // wins either way.
    if (!g_busy_since_ns.compare_exchange_strong(since, now,
                                                 std::memory_order_relaxed)) {
      // |since| now holds the other caller's stamp.
    } else {
      since = now;
    }
  }
  if (now - since >= std::uint64_t{busy} * kNsPerMs) return false;
  g_renames_refused.fetch_add(1, std::memory_order_relaxed);
  return true;
}

}  // namespace internal

}  // namespace se::fs
