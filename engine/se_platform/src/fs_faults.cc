// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/fs_faults.h"

#include <SDL3/SDL_timer.h>

#include <atomic>
#include <cstdint>

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

}  // namespace

void SetFsFaults(const FsFaults& faults) noexcept {
  if constexpr (!kFsFaultHooks) return;
  g_busy_since_ns.store(0, std::memory_order_relaxed);
  g_write_delay_ms.store(faults.write_step_delay_ms, std::memory_order_relaxed);
  g_read_delay_ms.store(faults.read_step_delay_ms, std::memory_order_relaxed);
  g_rename_busy_ms.store(faults.rename_busy_ms, std::memory_order_release);
}

void ClearFsFaults() noexcept { SetFsFaults(FsFaults{}); }

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

bool RefuseReplace() noexcept {
  if constexpr (!kFsFaultHooks) return false;
  const std::uint32_t busy = g_rename_busy_ms.load(std::memory_order_acquire);
  if (busy == 0) return false;
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
