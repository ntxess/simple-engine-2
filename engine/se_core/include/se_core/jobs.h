// Copyright 2026 The simple-engine-2 Authors.
//
// Fork-join parallelism inside a tick (docs/design/mt-design.md "Job
// System"). A system declares a ForkSite and calls ParallelFor with a count;
// the pool splits [0, count) into ceil(count / grain) chunks whose
// boundaries depend only on (count, grain), so results never depend on how
// many threads run them.
//
// ParallelFor runs the chunks inline, in chunk order on the calling thread,
// when the site is disabled, inside a fallback window, below min_items, when
// the pool has no workers (W = 0) or when the chunks give fewer than two
// participants; an exception thrown by an inline chunk (a third-party throw
// such as bad_alloc) propagates to the caller's fault boundary. Otherwise
// (M14) it forks: participants = min(W + 1, chunks / 4), the caller and
// participants - 1 woken workers claim batches of chunks, the caller joins,
// and the first exception a chunk threw is rethrown on the caller after the
// join (each threaded chunk runs inside the pool's fault boundary,
// src/fork_fault_boundary.cc; ADR-006, ADR-107).
//
// The pool creates no thread itself (se_core may not, ADR-029): its
// WorkerThreads (se_os/fork_workers.h) start the workers as engine threads
// of role kForkWorker. No worker exists until a site is enabled; the shipped
// configurations enable none (design change 23; M16 decides).
//
// Sites: a system declares a ForkSiteDecl and asks the pool for its record,
// which carries the site's enabled flag, fallback window and ForkStats:
//
//   jobs::ForkSite& site = context.jobs.Site(MySiteDecl());
//   auto view = world.Parallel<Transform, Mover>(site);
//   context.jobs.ParallelFor(site, view.size(), [&](jobs::ChunkRange r) {
//     view.ForChunk(r, [&](entt::entity e) { ... });
//   });
//
// A site the caller owns (a ForkSite object, as in M2) works too; only the
// pool's own sites are listed by Sites() and learn the spin prediction.
//
// Variable outputs go into ChunkOutputs, one arena per chunk, concatenated
// in chunk order after the join, so they are the same for every W.
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
// thread, and several workers may run the same nested site at once). The
// breadcrumb shows the innermost running chunk and returns to the outer
// one when the nested fork ends.

#ifndef SE_CORE_JOBS_H_
#define SE_CORE_JOBS_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/core/fwd.hpp>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "se_core/assert.h"
#include "se_core/breadcrumb.h"
#include "se_core/error.h"
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

// The most worker threads a pool runs (mt-design "Threads": W_max <= 15).
inline constexpr int kMaxWorkers = 15;
// Join times kept per site for its percentiles.
inline constexpr std::size_t kJoinRingSize = 2048;
// A worker that sees its wake later than this is counted as a late wake
// (the M14 soak's bound, ADR-110).
inline constexpr std::uint64_t kLateWakeNs = 2'000'000;

struct ForkStats {
  // ParallelFor calls, and the chunks they had.
  std::uint64_t forks = 0;
  std::uint64_t chunks = 0;
  // Forks run inline (disabled, below min_items, W = 0, too few chunks for
  // two participants, a fallback window, no workers) and threaded.
  std::uint64_t inline_forks = 0;
  std::uint64_t threaded_forks = 0;
  // Fallback windows opened (a join slower than 4x its median), and forks
  // run inline inside one.
  std::uint64_t fallbacks = 0;
  std::uint64_t fallback_forks = 0;
  // Workers woken for a threaded fork that ran none of its chunks: their
  // wake came after every chunk was claimed.
  std::uint64_t missed_wakes = 0;
  // Workers woken for a threaded fork that saw the wake more than
  // kLateWakeNs after it was sent, and still ran a chunk of it.
  std::uint64_t late_wakes = 0;
  // Threaded forks whose chunk threw (rethrown after the join).
  std::uint64_t faults = 0;
  // The last threaded fork's participants (the caller and the woken
  // workers), and the most of any.
  std::uint32_t participants = 0;
  std::uint32_t max_participants = 0;
  // Threaded joins (publish to join) over the last kJoinRingSize, in
  // microseconds: refreshed after the 32nd join, every 64 joins and by
  // ForkJoinPool::RefreshStats().
  double join_p50_us = 0.0;
  double join_p99_us = 0.0;
  double join_p999_us = 0.0;
  // Over every threaded join.
  double join_max_us = 0.0;
};

// The durations of a site's last kJoinRingSize threaded joins.
class JoinTimes {
 public:
  void Record(std::uint64_t ns) noexcept;
  [[nodiscard]] std::uint32_t Count() const noexcept { return count_; }
  // Nearest-rank percentile in nanoseconds; 0 when empty.
  [[nodiscard]] std::uint64_t PercentileNs(double p) const;
  // The kept durations, ascending.
  void Sorted(std::vector<std::uint32_t>& out) const;

 private:
  std::array<std::uint32_t, kJoinRingSize> ring_ns_{};
  std::uint32_t head_ = 0;
  std::uint32_t count_ = 0;
};

enum class CoreClass : std::uint8_t { kAnyPhysical, kPerformanceOnly };

// What a system declares about its fork site; the pool keeps the rest
// (ForkJoinPool::Site). Declarations live in static storage (a namespace-
// scope constant or a function-local static const) and are named uniquely
// ("<module>.<system>").
struct ForkSiteDecl {
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
};

class ForkJoinPool;

// A fork site: its declaration, whether it may fork, its fallback window
// and its stats. Owned by the pool (ForkJoinPool::Site) or by the caller.
// Owner thread only.
struct ForkSite {
  std::string_view name{};
  std::uint32_t grain = 1024;
  std::uint32_t min_items = 0;
  CoreClass cores = CoreClass::kAnyPhysical;
  std::span<const entt::id_type> reads{};
  std::span<const entt::id_type> writes{};
  // Sites ship disabled (design change 23).
  bool enabled = false;
  // A slow join runs the site inline until this time (the pool's clock).
  std::uint64_t inline_until_ns = 0;
  ForkStats stats{};
  // The pool's bookkeeping: the threaded joins, the median the fallback
  // rule compares with, whether the next fork after this site's last one
  // came within the spin window (the workers' spin prediction), and the
  // pool that owns the record (null for a caller's site).
  JoinTimes joins{};
  std::uint64_t median_ns = 0;
  bool followed_quickly = false;
  const ForkJoinPool* owner = nullptr;
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
// Taken once per fork by each thread that runs its chunks (the caller and
// every participating worker); a nested fork does not take it again.
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

// Per-chunk outputs (mt-design "Rules inside a fork"): one growable arena
// per chunk, which only that chunk writes during the fork, concatenated in
// chunk order after the join, so the result is the same for every W and
// claim order. The arenas keep their capacity between forks.
//
//   outputs.Reset(pool.ChunkCountFor(site, n));   // owner, before the fork
//   pool.ParallelFor(site, n, [&](jobs::ChunkRange r) {
//     outputs.At(r).push_back(...);               // the chunk's own arena
//   });
//   outputs.AppendTo(all);                        // owner, after the join
template <class T>
class ChunkOutputs {
 public:
  // Owner thread, before the fork: |chunks| empty arenas.
  void Reset(std::uint32_t chunks) {
    if (arenas_.size() < chunks) arenas_.resize(chunks);
    for (std::uint32_t i = 0; i < chunks; ++i) arenas_[i].clear();
    used_ = chunks;
  }
  // Inside the fork: the running chunk's arena.
  [[nodiscard]] std::vector<T>& At(const ChunkRange& range) {
    SE_ASSERT_MSG(range.chunk < used_, "ChunkOutputs: chunk not reset");
    return arenas_[range.chunk];
  }
  [[nodiscard]] const std::vector<T>& Chunk(std::uint32_t chunk) const {
    return arenas_[chunk];
  }
  [[nodiscard]] std::uint32_t Chunks() const noexcept { return used_; }
  // Owner thread, after the join: the items of every chunk.
  [[nodiscard]] std::size_t Total() const noexcept {
    std::size_t total = 0;
    for (std::uint32_t i = 0; i < used_; ++i) total += arenas_[i].size();
    return total;
  }
  // Owner thread, after the join: appends every chunk's items to |out| in
  // chunk order.
  void AppendTo(std::vector<T>& out) const {
    out.reserve(out.size() + Total());
    for (std::uint32_t i = 0; i < used_; ++i) {
      out.insert(out.end(), arenas_[i].begin(), arenas_[i].end());
    }
  }

 private:
  std::vector<std::vector<T>> arenas_;
  std::uint32_t used_ = 0;
};

// The index of the pool worker the calling thread is (0 .. W - 1), or -1
// on any other thread (the caller of a fork included).
[[nodiscard]] int CurrentWorker() noexcept;

// Where a worker runs, from WorkerThreads::Placement.
struct WorkerSpec {
  int index = 0;
  // kPerformanceOnly: the P-cores on main's last-level cache (the workers a
  // PerformanceOnly site uses are the first PerformanceOnlyWorkers()).
  CoreClass placement = CoreClass::kAnyPhysical;
  // The efficiency class of the cores the worker prefers (higher is
  // faster; calibrated per class when the pool starts).
  std::uint8_t efficiency_class = 0;
};

// Starts and joins the pool's worker threads. se_os's ForkWorkers does it
// with StartThread (role kForkWorker, Qos::kHigh, soft affinity); tests may
// supply their own. Called on the pool's owner thread only.
class WorkerThreads {
 public:
  WorkerThreads() = default;
  virtual ~WorkerThreads() = default;
  WorkerThreads(const WorkerThreads&) = delete;
  WorkerThreads& operator=(const WorkerThreads&) = delete;

  // W_max for this machine (ForkPoolSize), and the workers a
  // PerformanceOnly site may use (PerformanceOnlyCount).
  [[nodiscard]] virtual int MaxWorkers() const = 0;
  [[nodiscard]] virtual int PerformanceOnlyWorkers() const = 0;
  // Where worker |index| of |count| runs.
  [[nodiscard]] virtual WorkerSpec Placement(int index, int count) const = 0;
  // Starts the worker |spec| describes running |body|, which returns once
  // the pool stops.
  virtual std::expected<void, Error> Start(
      const WorkerSpec& spec, std::move_only_function<void()> body) = 0;
  // Joins every started worker (the pool has told their bodies to return).
  virtual void JoinAll() noexcept = 0;
  // True while the machine runs on battery: the workers never spin then.
  [[nodiscard]] virtual bool OnBattery() const = 0;
};

// Receives every worker wake (the wake-stall soak; nullptr otherwise).
// Called on the worker that woke; each worker passes its own index.
class WakeTrace {
 public:
  WakeTrace() = default;
  virtual ~WakeTrace() = default;
  WakeTrace(const WakeTrace&) = delete;
  WakeTrace& operator=(const WakeTrace&) = delete;

  // |bump_ns|: when the caller bumped the worker's wake word (the oldest
  // bump it had not seen); |woke_ns|: when the worker saw it. Both on the
  // pool's clock (ProfileNowNs: QueryPerformanceCounter on Windows).
  virtual void Record(int worker, std::uint64_t bump_ns,
                      std::uint64_t woke_ns) noexcept = 0;
};

// One efficiency class's startup calibration: a dummy fork of about 10 ms
// run by that class's workers only.
struct Calibration {
  std::uint8_t efficiency_class = 0;
  int workers = 0;
  // Dummy chunks the class's workers ran, their median and longest time.
  std::uint32_t chunks = 0;
  double chunk_p50_us = 0.0;
  double chunk_max_us = 0.0;
  double wall_ms = 0.0;
  // The caller's time for one dummy chunk over this class's median: 1 on
  // the caller's class, below 1 on slower cores.
  double relative_speed = 0.0;
};

// The pool's own counters (owner thread; the workers' parts are read with
// relaxed loads, so they may trail by a fork).
struct PoolStats {
  // Workers started (0 until a site is enabled), and the most a
  // PerformanceOnly site uses.
  int workers = 0;
  int performance_only_workers = 0;
  bool start_failed = false;
  std::uint64_t threaded_forks = 0;
  // Wake bumps sent, and what the workers saw of them.
  std::uint64_t bumps = 0;
  std::uint64_t wakes = 0;
  std::uint64_t missed_wakes = 0;
  std::uint64_t late_wakes = 0;
  // Wakes whose bump stamp a racing bump had taken (latency unknown).
  std::uint64_t unstamped_wakes = 0;
  std::uint64_t wake_latency_max_ns = 0;
  // Chunks the workers ran (the caller's are the rest).
  std::uint64_t worker_chunks = 0;
  // Spins after a join, and those that found the next fork.
  std::uint64_t spins = 0;
  std::uint64_t spin_hits = 0;
};

// A worker's wake word and the value it has accounted for: equal once it
// saw every bump (the no-lost-wake property: never stays unequal).
struct WorkerWake {
  std::uint32_t word = 0;
  std::uint32_t seen = 0;
  std::uint8_t efficiency_class = 0;
};

class ForkJoinPool {
 public:
  struct Config {
    // Worker threads (W): 0 runs every fork inline (M2's pool and the
    // default); -1 is what the machine supports (WorkerThreads::
    // MaxWorkers, capped by max_workers); N > 0 is N (at most kMaxWorkers,
    // and max_workers when it is set; tests may exceed the machine's size).
    // Workers start only when a site is enabled.
    int workers = 0;
    // The machine's W_max (ForkPoolSize): physical cores - 2 (homogeneous
    // 8+ cores: - 1), at most 15; -1 = no cap beyond the WorkerThreads'.
    int max_workers = -1;
    // Workers a PerformanceOnly site may use; -1 = the WorkerThreads'
    // PerformanceOnlyWorkers.
    int performance_only_workers = -1;
    // After a join a worker spins at most this long when another fork is
    // predicted, never on battery; 0 = never.
    std::uint32_t spin_us = 20;
    // SE_CHAOS_SEED: seeded claim permutations, yields, delays and late
    // wakes (ADR-109); 0 = off. Results never depend on it.
    std::uint64_t chaos_seed = 0;
    // Makes the worker threads (se_os MakeForkWorkers); empty: the pool
    // never starts any and every fork runs inline.
    std::function<std::unique_ptr<WorkerThreads>()> threads{};
    // Sites enabled when the pool registers them; "*" enables every site
    // (tests only). Shipped configurations list none.
    std::vector<std::string> enabled_sites{};
    // The fallback rule: a threaded join slower than fallback_factor times
    // its median runs the site inline for fallback_ns, then it retries;
    // fallback_factor 0 turns the rule off (tests).
    std::uint32_t fallback_factor = 4;
    std::uint64_t fallback_ns = 5'000'000'000;
    // A 10 ms dummy fork per efficiency class when the workers start.
    bool calibrate = true;
    // Tests: the grain of every site (0 = the site's own), so small scenes
    // still give many chunks; the chunk boundaries still depend only on
    // (count, grain), and every W of a test uses the same value.
    std::uint32_t grain_override = 0;
    // participants = min(W + 1, chunks / chunks_per_participant).
    std::uint32_t chunks_per_participant = 4;
    // The clock of join times, fallback windows and wake stamps
    // (nanoseconds); nullptr = ProfileNowNs. Tests replace it.
    std::uint64_t (*now_ns)() = nullptr;
    // Every worker wake goes here (the soak); not owned.
    WakeTrace* wake_trace = nullptr;
  };

  ForkJoinPool() noexcept;
  explicit ForkJoinPool(Config config) noexcept;
  // Stops and joins the workers.
  ~ForkJoinPool();
  ForkJoinPool(const ForkJoinPool&) = delete;
  ForkJoinPool& operator=(const ForkJoinPool&) = delete;
  ForkJoinPool(ForkJoinPool&&) = delete;
  ForkJoinPool& operator=(ForkJoinPool&&) = delete;

  // Runs body(ChunkRange) for every chunk of [0, count): inline in chunk
  // order, or forked across the workers (see the file comment). Chunks must
  // not make structural World calls; they hold g_fork_chunk, and fork again
  // only through NestedParallelFor. Exactly once per chunk, unless a chunk
  // throws: then the chunks not yet started are skipped and the first
  // exception propagates (after the join, when forked).
  template <class F>
  void ParallelFor(ForkSite& site, std::uint32_t count, F&& body)
      SE_REQUIRES(g_sim_owner);

  // The pool's record of |decl|'s site (by name), made at the first call,
  // enabled when Config::enabled_sites names it.
  [[nodiscard]] ForkSite& Site(const ForkSiteDecl& decl)
      SE_REQUIRES(g_sim_owner);
  // Enables or disables the site |name|, now (if registered) and when it is
  // registered later. Enabling starts the workers.
  void EnableSite(std::string_view name, bool enabled) SE_REQUIRES(g_sim_owner);
  // The pool's sites in registration order.
  [[nodiscard]] std::vector<const ForkSite*> Sites() const;
  // The pool's record of the site |name|, or nullptr.
  [[nodiscard]] const ForkSite* FindSite(std::string_view name) const;
  // Recomputes every pool site's join percentiles now.
  void RefreshStats() SE_REQUIRES(g_sim_owner);
  // The chunks ParallelFor makes of |count| items of |site| (the grain
  // override included), for sizing ChunkOutputs.
  [[nodiscard]] std::uint32_t ChunkCountFor(const ForkSite& site,
                                            std::uint32_t count) const noexcept;

  // Starts the workers now (EnableSite and the first enabled fork do it).
  // False when the pool has none to start (W = 0, no WorkerThreads) or they
  // failed to start; it then stays inline.
  bool StartWorkers() SE_REQUIRES(g_sim_owner);

  [[nodiscard]] const Config& Configuration() const noexcept { return config_; }
  // Threads that run chunks besides the caller: 0 until the workers start.
  [[nodiscard]] int ActiveWorkers() const noexcept;
  [[nodiscard]] PoolStats Stats() const;
  // Each worker's wake word and what it saw (tests, the soak).
  [[nodiscard]] std::vector<WorkerWake> WorkerWakes() const;
  [[nodiscard]] std::span<const Calibration> Calibrations() const noexcept {
    return calibrations_;
  }

 private:
  struct Shared;
  using InvokeFn = void (*)(const void* body, ChunkRange range);
  struct Plan {
    std::uint32_t count = 0;
    std::uint32_t grain = 0;
    std::uint32_t chunks = 0;
    std::uint32_t participants = 1;
  };

  template <class Body>
  static void InvokeChunk(const void* body, ChunkRange range)
      SE_REQUIRES(g_fork_chunk) {
    (*static_cast<Body*>(const_cast<void*>(body)))(range);
  }

  [[nodiscard]] Plan PlanFork(ForkSite& site, std::uint32_t count)
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::uint32_t Participants(ForkSite& site, const Plan& plan)
      SE_REQUIRES(g_sim_owner);
  void RunThreaded(ForkSite& site, const Plan& plan, InvokeFn invoke,
                   const void* body) SE_REQUIRES(g_sim_owner);
  void RecordJoin(ForkSite& site, std::uint64_t join_ns, std::uint64_t end_ns,
                  std::span<const int> woken, std::uint64_t generation);
  void Calibrate() SE_REQUIRES(g_sim_owner);
  void StopWorkers() noexcept;
  [[nodiscard]] bool SiteEnabledByConfig(std::string_view name) const;
  [[nodiscard]] std::uint64_t Now() const noexcept;

  Config config_;
  std::unique_ptr<WorkerThreads> threads_;
  // The workers' shared state, made when they start.
  std::unique_ptr<Shared> shared_;
  int performance_only_ = 0;
  bool start_failed_ = false;
  std::vector<std::unique_ptr<ForkSite>> sites_;
  // The declaration each site was registered from (a quick match before
  // the name), and EnableSite's choices for sites registered later.
  std::vector<const ForkSiteDecl*> site_decls_;
  std::vector<std::pair<std::string, bool>> site_overrides_;
  std::vector<Calibration> calibrations_;
  // The spin prediction: the last threaded fork's pool site and its end.
  ForkSite* last_site_ = nullptr;
  std::uint64_t last_join_end_ns_ = 0;
  // The battery state, polled at most once a second.
  bool on_battery_ = false;
  std::uint64_t battery_checked_ns_ = 0;
  std::uint64_t threaded_forks_ = 0;
  std::uint64_t missed_wakes_ = 0;
  std::uint64_t late_wakes_ = 0;
};

template <class F>
void ForkJoinPool::ParallelFor(ForkSite& site, std::uint32_t count, F&& body) {
  SE_ASSERT_MSG(site.grain > 0, "a fork site needs a grain");
  SE_ASSERT_MSG(!t_in_fork,
                "ParallelFor inside a fork chunk: use jobs::NestedParallelFor");
  SE_PROFILE_ZONE_DYNAMIC(site.name);
  auto&& run_chunk = std::forward<F>(body);
  const Plan plan = PlanFork(site, count);
  if (plan.participants < 2) {
    const ChunkCrumbScope crumb;
    const ForkScope scope;
    for (std::uint32_t chunk = 0; chunk < plan.chunks; ++chunk) {
      crumb.Set(chunk);
      run_chunk(ChunkAt(count, plan.grain, chunk));
    }
    return;
  }
  using Body = std::remove_reference_t<decltype(run_chunk)>;
  RunThreaded(site, plan, &InvokeChunk<Body>,
              static_cast<const void*>(std::addressof(run_chunk)));
}

// A fork inside a fork chunk: body(ChunkRange) for every chunk of
// [0, count), inline on the calling thread in chunk order, with the same
// chunking as ParallelFor (the site's own grain). The site's stats are not
// touched.
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
