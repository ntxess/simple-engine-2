// Copyright 2026 The simple-engine-2 Authors.
//
// Fork-join parallelism inside a tick (docs/design/mt-design.md "Job
// System"). A system declares a ForkSite and calls ParallelFor with a count;
// the pool splits [0, count) into ceil(count / grain) chunks whose
// boundaries depend only on (count, grain), so results never depend on how
// many threads run them.
//
// M2 ships the inline pool only (W = 0): the chunks run in order on the
// calling thread with t_in_fork set, and an exception thrown by a chunk (a
// third-party throw such as bad_alloc) propagates to the caller's fault
// boundary; the pool itself has no catch. M14's threaded pool keeps this
// interface and chunking, catches on the workers at its fault boundary and
// rethrows on the owner thread.
//
// Nested forks run inline (ADR-019). A chunk does not hold g_sim_owner, so
// it cannot call ParallelFor; it holds g_fork_chunk instead, and a chunk
// lambda that forks again says so and calls NestedParallelFor:
//
//   pool.ParallelFor(site, n, [&](jobs::ChunkRange r)
//                                 SE_REQUIRES(jobs::g_fork_chunk) {
//     jobs::NestedParallelFor(inner, m, [&](jobs::ChunkRange s) { ... });
//   });
//
// A nested fork leaves its site's stats alone (they belong to the owner
// thread, and in M14 several workers may run the same nested site at once).
// The breadcrumb shows the innermost running chunk and returns to the outer
// one when the nested fork ends.

#ifndef SE_CORE_JOBS_H_
#define SE_CORE_JOBS_H_

#include <algorithm>
#include <cstdint>
#include <entt/core/fwd.hpp>
#include <span>
#include <string_view>
#include <utility>

#include "se_core/assert.h"
#include "se_core/breadcrumb.h"
#include "se_core/profile.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se::jobs {

// True while the calling thread runs a fork chunk. World structural calls
// and World-level sinks assert it is false (SE_ASSERT_NOT_IN_FORK).
inline thread_local bool t_in_fork = false;

// The capability of code running inside a fork chunk (ADR-019): ParallelFor
// holds it while it runs the chunks, and NestedParallelFor requires it.
struct SE_CAPABILITY("role") ForkChunkRole {};
extern ForkChunkRole g_fork_chunk;

// One chunk: items [begin, end).
struct ChunkRange {
  std::uint32_t chunk = 0;
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
};

struct ForkStats {
  std::uint64_t forks = 0;
  std::uint64_t inline_forks = 0;
  std::uint64_t chunks = 0;
  // Threaded pool (M14): inline fallbacks after a slow join, late and missed
  // wakes, and join percentiles.
  std::uint64_t fallbacks = 0;
  std::uint64_t late_wakes = 0;
  std::uint64_t missed_wakes = 0;
  double join_p50_us = 0.0;
  double join_p999_us = 0.0;
  double join_max_us = 0.0;
};

enum class CoreClass : std::uint8_t { kAnyPhysical, kPerformanceOnly };

// A fork site, registered by its system.
struct ForkSite {
  std::string_view name{};
  // Items per chunk: at least 10 us of measured work.
  std::uint32_t grain = 1024;
  // Below this many items the site runs inline.
  std::uint32_t min_items = 0;
  CoreClass cores = CoreClass::kAnyPhysical;
  // EnTT storage ids the chunks read and write; ParallelView rejects any
  // other access in debug builds.
  std::span<const entt::id_type> reads{};
  std::span<const entt::id_type> writes{};
  // Sites ship disabled (design change 23).
  bool enabled = false;
  // Threaded pool: a slow join runs the site inline until this time.
  std::uint64_t inline_until_ns = 0;
  ForkStats stats{};
};

// The number of chunks for |count| items: ceil(count / grain).
[[nodiscard]] constexpr std::uint32_t ChunkCount(std::uint32_t count,
                                                 std::uint32_t grain) noexcept {
  if (grain == 0) return 0;
  return static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(count) + grain - 1) / grain);
}

// Chunk |chunk| of |count| items.
[[nodiscard]] constexpr ChunkRange ChunkAt(std::uint32_t count,
                                           std::uint32_t grain,
                                           std::uint32_t chunk) noexcept {
  const std::uint64_t begin = static_cast<std::uint64_t>(chunk) * grain;
  const std::uint64_t end = std::min<std::uint64_t>(count, begin + grain);
  return {chunk, static_cast<std::uint32_t>(begin),
          static_cast<std::uint32_t>(end)};
}

// Marks the calling thread as running fork chunks and holds g_fork_chunk.
// Taken once per fork by the thread that runs its chunks (the caller in M2,
// each worker in M14); a nested fork does not take it again.
class SE_SCOPED_CAPABILITY ForkScope {
 public:
  ForkScope() noexcept SE_ACQUIRE(g_fork_chunk);
  ~ForkScope() SE_RELEASE(g_fork_chunk);
  ForkScope(const ForkScope&) = delete;
  ForkScope& operator=(const ForkScope&) = delete;

 private:
  bool previous_ = t_in_fork;
};

// Shows fork chunks in the calling thread's breadcrumb and restores the
// chunk it showed before (an outer fork's chunk, or 0) when it ends.
class ChunkCrumbScope {
 public:
  ChunkCrumbScope() noexcept
      : crumb_(ThreadBreadcrumb()), previous_(crumb_.Fields().chunk) {}
  ~ChunkCrumbScope() { crumb_.SetChunk(previous_); }
  ChunkCrumbScope(const ChunkCrumbScope&) = delete;
  ChunkCrumbScope& operator=(const ChunkCrumbScope&) = delete;

  void Set(std::uint32_t chunk) const noexcept { crumb_.SetChunk(chunk); }

 private:
  Breadcrumb& crumb_;
  std::uint16_t previous_;
};

class ForkJoinPool {
 public:
  struct Config {
    // Worker threads; the shipped default is none.
    int workers = 0;
    // Physical cores - 2 (homogeneous 8+ cores: - 1), at most 15; -1 = auto.
    int max_workers = -1;
    std::uint32_t spin_us = 20;
    // SE_CHAOS_SEED: seeded claim permutations and delays (threaded pool).
    std::uint64_t chaos_seed = 0;
  };

  ForkJoinPool() noexcept;
  explicit ForkJoinPool(const Config& config) noexcept;

  // Runs body(ChunkRange) for every chunk of [0, count), in chunk order on
  // the calling thread (M2). Chunks must not make structural World calls;
  // they hold g_fork_chunk, and fork again only through NestedParallelFor.
  template <class F>
  void ParallelFor(ForkSite& site, std::uint32_t count, F&& body)
      SE_REQUIRES(g_sim_owner);

  [[nodiscard]] const Config& Configuration() const noexcept { return config_; }
  // Threads that run chunks besides the caller: 0 in M2.
  [[nodiscard]] int ActiveWorkers() const noexcept { return 0; }

 private:
  Config config_;
};

template <class F>
void ForkJoinPool::ParallelFor(ForkSite& site, std::uint32_t count, F&& body) {
  SE_ASSERT_MSG(site.grain > 0, "a fork site needs a grain");
  SE_ASSERT_MSG(!t_in_fork,
                "ParallelFor inside a fork chunk: use jobs::NestedParallelFor");
  SE_PROFILE_ZONE_DYNAMIC(site.name);
  auto&& run_chunk = std::forward<F>(body);
  const std::uint32_t chunks = ChunkCount(count, site.grain);
  ++site.stats.forks;
  ++site.stats.inline_forks;
  site.stats.chunks += chunks;
  const ChunkCrumbScope crumb;
  const ForkScope scope;
  for (std::uint32_t chunk = 0; chunk < chunks; ++chunk) {
    crumb.Set(chunk);
    run_chunk(ChunkAt(count, site.grain, chunk));
  }
}

// A fork inside a fork chunk: body(ChunkRange) for every chunk of
// [0, count), inline on the calling thread in chunk order, with the same
// chunking as ParallelFor. The site's stats are not touched.
template <class F>
void NestedParallelFor(const ForkSite& site, std::uint32_t count, F&& body)
    SE_REQUIRES(g_fork_chunk) {
  SE_ASSERT_MSG(site.grain > 0, "a fork site needs a grain");
  SE_ASSERT_MSG(t_in_fork,
                "NestedParallelFor outside a fork chunk: use "
                "ForkJoinPool::ParallelFor");
  SE_PROFILE_ZONE_DYNAMIC(site.name);
  auto&& run_chunk = std::forward<F>(body);
  const std::uint32_t chunks = ChunkCount(count, site.grain);
  const ChunkCrumbScope crumb;
  for (std::uint32_t chunk = 0; chunk < chunks; ++chunk) {
    crumb.Set(chunk);
    run_chunk(ChunkAt(count, site.grain, chunk));
  }
}

}  // namespace se::jobs

#endif  // SE_CORE_JOBS_H_
