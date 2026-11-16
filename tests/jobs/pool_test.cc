// Copyright 2026 The simple-engine-2 Authors.
//
// The threaded ForkJoinPool's properties (docs/milestone-specs.md M14;
// ADR-107 to ADR-109), label unit, and under TSan in linux-clang-tsan:
//   - no worker exists until a site is enabled; workers are kForkWorker
//     engine threads;
//   - every chunk runs exactly once, for W = 1/3/7 and the chaos seeds;
//   - a join sees every chunk's writes, and the next fork sees them too;
//   - no lost wake: forks whose chunks wait for every participant finish
//     (a worker that slept through its bump would hang them), and every
//     worker's wake word is eventually seen;
//   - the first exception is rethrown after the join, later chunks are
//     skipped, and the pool works on afterwards;
//   - destroying a pool joins its workers, parked or spinning;
//   - nested forks run inline on the worker; per-chunk outputs concatenate
//     in chunk order; PerformanceOnly sites use their workers only;
//   - ForkStats: participants, join percentiles; the 4x-median fallback
//     window and its expiry (on a test clock); the spin after a join only
//     when the next fork is predicted and never on battery; the startup
//     calibration of each efficiency class.

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/jobs.h"
#include "se_core/profile_clock.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_os/thread.h"
#include "support/fork_config.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::ForkTestConfig;
using testing::ForkTestOptions;
using testing::kChaosSeeds;

// The seeds a property runs under: no chaos, then the matrix's eight.
std::vector<std::uint64_t> SeedsWithNone() {
  std::vector<std::uint64_t> seeds = {0};
  seeds.insert(seeds.end(), kChaosSeeds.begin(), kChaosSeeds.end());
  return seeds;
}

ForkTestOptions Options(std::uint64_t seed) {
  ForkTestOptions options;
  options.chaos_seed = seed;
  return options;
}

int LiveForkWorkers() {
  const ThreadRegistry& registry = ThreadRegistry::Instance();
  int live = 0;
  for (std::uint32_t slot = 0; slot < registry.SlotsUsed(); ++slot) {
    const ThreadSlotInfo info = registry.Read(slot);
    if (info.state == ThreadSlotState::kLive &&
        info.role == ThreadRole::kForkWorker) {
      ++live;
    }
  }
  return live;
}

// Workers take their registry slot when their thread starts running, a
// little after StartThread returns: waits (at most 10 s) for |count|.
bool WaitForLiveForkWorkers(int count) {
  const std::uint64_t start = ProfileNowNs();
  while (LiveForkWorkers() != count) {
    if (ProfileNowNs() - start > 10'000'000'000) return false;
    std::this_thread::yield();
  }
  return true;
}

const jobs::ForkSiteDecl& TestSite() {
  static const jobs::ForkSiteDecl kSite{.name = "test.site", .grain = 1};
  return kSite;
}

// The fallback tests' clock: chunks advance it, so a join lasts exactly
// what its chunks add; a battery poll of TestWorkerThreads takes
// g_test_poll_ns of it.
std::atomic<std::uint64_t> g_test_now{1'000'000'000};
std::atomic<std::uint64_t> g_test_poll_ns{0};
std::uint64_t TestNow() { return g_test_now.load(); }

// Worker threads with chosen efficiency classes and battery state, started
// with se_os StartThread.
class TestWorkerThreads final : public jobs::WorkerThreads {
 public:
  TestWorkerThreads(std::vector<std::uint8_t> classes, bool on_battery,
                    int performance_only)
      : classes_(std::move(classes)),
        on_battery_(on_battery),
        performance_only_(performance_only) {}
  ~TestWorkerThreads() override { JoinAll(); }

  [[nodiscard]] int MaxWorkers() const override {
    return static_cast<int>(classes_.size());
  }
  [[nodiscard]] int PerformanceOnlyWorkers() const override {
    return performance_only_;
  }
  [[nodiscard]] jobs::WorkerSpec Placement(int index,
                                           int /*count*/) const override {
    jobs::WorkerSpec spec;
    spec.index = index;
    spec.placement = index < performance_only_
                         ? jobs::CoreClass::kPerformanceOnly
                         : jobs::CoreClass::kAnyPhysical;
    spec.efficiency_class =
        classes_[static_cast<std::size_t>(index) % classes_.size()];
    return spec;
  }
  std::expected<void, Error> Start(
      const jobs::WorkerSpec& /*spec*/,
      std::move_only_function<void()> body) override {
    auto thread = StartThread(
        {.role = ThreadRole::kForkWorker, .name = "test-worker"},
        [run = std::move(body)](const std::stop_token&) mutable { run(); });
    if (!thread) return std::unexpected(std::move(thread.error()));
    threads_.push_back(std::move(*thread));
    return {};
  }
  void JoinAll() noexcept override {
    for (Thread& thread : threads_) thread.Join();
    threads_.clear();
  }
  [[nodiscard]] bool OnBattery() const override {
    g_test_now.fetch_add(g_test_poll_ns.load());
    return on_battery_;
  }

 private:
  std::vector<std::uint8_t> classes_;
  bool on_battery_;
  int performance_only_;
  std::vector<Thread> threads_;
};

jobs::ForkJoinPool::Config TestHostConfig(
    const std::vector<std::uint8_t>& classes, bool on_battery,
    int performance_only) {
  jobs::ForkJoinPool::Config config =
      ForkTestConfig(static_cast<int>(classes.size()));
  const auto shared =
      std::make_shared<const std::vector<std::uint8_t>>(classes);
  config.threads = [shared, on_battery, performance_only] {
    return std::make_unique<TestWorkerThreads>(*shared, on_battery,
                                               performance_only);
  };
  return config;
}

TEST_CASE("No worker exists until a site is enabled", "[jobs][pool]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  const int before = LiveForkWorkers();
  {
    jobs::ForkJoinPool::Config config = ForkTestConfig(3);
    config.enabled_sites.clear();
    jobs::ForkJoinPool pool(config);
    jobs::ForkSite& site = pool.Site(TestSite());
    REQUIRE_FALSE(site.enabled);
    REQUIRE(site.owner == &pool);
    pool.ParallelFor(site, 64, [](jobs::ChunkRange) {});
    REQUIRE(pool.ActiveWorkers() == 0);
    CHECK(LiveForkWorkers() == before);
    REQUIRE(site.stats.inline_forks == 1);
    REQUIRE(site.stats.threaded_forks == 0);

    pool.EnableSite("test.site", true);
    REQUIRE(site.enabled);
    REQUIRE(pool.ActiveWorkers() == 3);
    CHECK(WaitForLiveForkWorkers(before + 3));
    pool.ParallelFor(site, 64, [](jobs::ChunkRange) {});
    REQUIRE(site.stats.threaded_forks == 1);
    REQUIRE(site.stats.participants == 4);
    REQUIRE(pool.Sites().size() == 1);
    CHECK(pool.FindSite("test.site") == &site);
  }
  // The destructor joined them.
  CHECK(LiveForkWorkers() == before);
}

TEST_CASE("A pool with W = 0 or no worker threads never starts any",
          "[jobs][pool]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  jobs::ForkJoinPool::Config none = ForkTestConfig(0);
  jobs::ForkJoinPool zero(none);
  jobs::ForkSite& a = zero.Site(TestSite());
  REQUIRE(a.enabled);
  zero.ParallelFor(a, 100, [](jobs::ChunkRange) {});
  REQUIRE(zero.ActiveWorkers() == 0);
  REQUIRE(a.stats.inline_forks == 1);

  jobs::ForkJoinPool::Config no_threads = ForkTestConfig(3);
  no_threads.threads = nullptr;
  jobs::ForkJoinPool bare(no_threads);
  jobs::ForkSite& b = bare.Site(TestSite());
  bare.ParallelFor(b, 100, [](jobs::ChunkRange) {});
  REQUIRE(bare.ActiveWorkers() == 0);
  CHECK(bare.Stats().start_failed);
  REQUIRE(b.stats.inline_forks == 1);
}

TEST_CASE("Every chunk runs exactly once for every W and chaos seed",
          "[jobs][pool][property]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  for (const int workers : {1, 3, 7}) {
    for (const std::uint64_t seed : SeedsWithNone()) {
      INFO("W = " << workers << ", chaos seed " << seed);
      jobs::ForkJoinPool pool(ForkTestConfig(workers, Options(seed)));
      for (const std::uint32_t grain : {1U, 3U, 16U}) {
        static const jobs::ForkSiteDecl kOnce{.name = "test.once"};
        jobs::ForkSite& site = pool.Site(kOnce);
        site.grain = grain;
        for (const std::uint32_t count : {2U, 7U, 64U, 333U, 1000U}) {
          const std::uint32_t chunks = jobs::ChunkCount(count, grain);
          std::vector<std::atomic<int>> runs(chunks);
          // Plain writes, one slot per item: TSan sees a race if two
          // threads ran the same chunk at once.
          std::vector<std::uint32_t> items(count, 0);
          pool.ParallelFor(site, count, [&](jobs::ChunkRange r) {
            runs[r.chunk].fetch_add(1, std::memory_order_relaxed);
            for (std::uint32_t i = r.begin; i < r.end; ++i) items[i] += i + 1;
          });
          for (std::uint32_t c = 0; c < chunks; ++c) {
            REQUIRE(runs[c].load() == 1);
          }
          for (std::uint32_t i = 0; i < count; ++i) {
            REQUIRE(items[i] == i + 1);
          }
        }
      }
      const jobs::PoolStats stats = pool.Stats();
      CHECK(stats.workers == workers);
      REQUIRE(stats.threaded_forks > 0);
    }
  }
}

TEST_CASE("A join sees every chunk's writes, and so does the next fork",
          "[jobs][pool][property]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  for (const std::uint64_t seed : SeedsWithNone()) {
    INFO("chaos seed " << seed);
    jobs::ForkJoinPool pool(ForkTestConfig(3, Options(seed)));
    jobs::ForkSite& site = pool.Site(TestSite());
    constexpr std::uint32_t kCount = 512;
    std::vector<std::uint64_t> a(kCount, 0);
    std::vector<std::uint64_t> b(kCount, 0);
    for (std::uint64_t round = 1; round <= 40; ++round) {
      // Each item of b reads an item of a another chunk wrote last fork.
      pool.ParallelFor(site, kCount, [&](jobs::ChunkRange r) {
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
          a[i] = round * 1000 + i;
        }
      });
      pool.ParallelFor(site, kCount, [&](jobs::ChunkRange r) {
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
          b[i] = a[(i * 7 + 13) % kCount] + 1;
        }
      });
      for (std::uint32_t i = 0; i < kCount; ++i) {
        REQUIRE(b[i] == round * 1000 + (i * 7 + 13) % kCount + 1);
      }
    }
  }
}

// A fork whose chunks each wait until every participant has entered one:
// it finishes only if every woken worker woke.
bool RendezvousFork(jobs::ForkJoinPool& pool, jobs::ForkSite& site,
                    std::uint32_t participants) SE_REQUIRES(g_sim_owner) {
  std::atomic<std::uint32_t> arrived{0};
  std::atomic<bool> timed_out{false};
  constexpr std::uint64_t kTimeoutNs = 10'000'000'000;
  pool.ParallelFor(site, participants, [&](jobs::ChunkRange) {
    arrived.fetch_add(1, std::memory_order_acq_rel);
    const std::uint64_t start = ProfileNowNs();
    while (arrived.load(std::memory_order_acquire) < participants) {
      if (ProfileNowNs() - start > kTimeoutNs) {
        timed_out.store(true);
        return;
      }
      std::this_thread::yield();
    }
  });
  return !timed_out.load();
}

// Every worker eventually accounts for its wake word (none sleeps through
// a bump); false after |timeout_ns|.
bool AllWakesSeen(const jobs::ForkJoinPool& pool,
                  std::uint64_t timeout_ns = 10'000'000'000) {
  const std::uint64_t start = ProfileNowNs();
  while (true) {
    bool all = true;
    for (const jobs::WorkerWake& wake : pool.WorkerWakes()) {
      all = all && wake.word == wake.seen;
    }
    if (all) return true;
    if (ProfileNowNs() - start > timeout_ns) return false;
    std::this_thread::yield();
  }
}

TEST_CASE("No woken worker sleeps through its bump", "[jobs][pool][property]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  for (const int workers : {1, 3, 7}) {
    for (const std::uint64_t seed : SeedsWithNone()) {
      INFO("W = " << workers << ", chaos seed " << seed);
      jobs::ForkJoinPool pool(ForkTestConfig(workers, Options(seed)));
      jobs::ForkSite& site = pool.Site(TestSite());
      const auto participants = static_cast<std::uint32_t>(workers + 1);
      for (int round = 0; round < 60; ++round) {
        REQUIRE(RendezvousFork(pool, site, participants));
        REQUIRE(site.stats.participants == participants);
        // Every few rounds, a quiet moment: each worker has parked, so the
        // next bumps reach sleeping workers.
        if (round % 10 == 9) {
          REQUIRE(AllWakesSeen(pool));
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      }
      REQUIRE(AllWakesSeen(pool));
      // A woken worker always ran a chunk of a rendezvous fork.
      REQUIRE(site.stats.missed_wakes == 0);
    }
  }
}

TEST_CASE("The first exception is rethrown after the join",
          "[jobs][pool][property]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  for (const std::uint64_t seed : SeedsWithNone()) {
    INFO("chaos seed " << seed);
    jobs::ForkJoinPool pool(ForkTestConfig(3, Options(seed)));
    jobs::ForkSite& site = pool.Site(TestSite());
    for (int round = 0; round < 20; ++round) {
      constexpr std::uint32_t kChunks = 64;
      std::vector<std::atomic<int>> runs(kChunks);
      std::string caught;
      try {
        pool.ParallelFor(site, kChunks, [&](jobs::ChunkRange r) {
          runs[r.chunk].fetch_add(1);
          if (r.chunk == 5 || r.chunk == 40) {
            throw std::runtime_error("chunk " + std::to_string(r.chunk));
          }
        });
      } catch (const std::runtime_error& error) {
        caught = error.what();
      }
      CHECK((caught == "chunk 5" || caught == "chunk 40"));
      // No chunk ran twice; the thrower ran; later ones may be skipped.
      int ran = 0;
      for (std::uint32_t c = 0; c < kChunks; ++c) {
        REQUIRE(runs[c].load() <= 1);
        ran += runs[c].load();
      }
      REQUIRE(ran >= 1);
      // The pool works on: every chunk once.
      std::vector<std::atomic<int>> again(kChunks);
      pool.ParallelFor(site, kChunks, [&](jobs::ChunkRange r) {
        again[r.chunk].fetch_add(1);
      });
      for (std::uint32_t c = 0; c < kChunks; ++c) {
        REQUIRE(again[c].load() == 1);
      }
    }
    REQUIRE(site.stats.faults == 20);
  }
}

TEST_CASE("A worker's throw reaches the caller", "[jobs][pool]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  jobs::ForkJoinPool pool(ForkTestConfig(3));
  jobs::ForkSite& site = pool.Site(TestSite());
  // The rendezvous makes each participant run one chunk; only workers
  // throw.
  std::atomic<std::uint32_t> arrived{0};
  std::string caught;
  try {
    pool.ParallelFor(site, 4, [&](jobs::ChunkRange) {
      arrived.fetch_add(1);
      while (arrived.load() < 4) std::this_thread::yield();
      if (jobs::CurrentWorker() >= 0) throw std::out_of_range("worker");
    });
  } catch (const std::out_of_range& error) {
    caught = error.what();
  }
  REQUIRE(caught == "worker");
  CHECK_FALSE(jobs::t_in_fork);
}

TEST_CASE("Destroying a pool joins its workers, parked or spinning",
          "[jobs][pool][property]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  const int before = LiveForkWorkers();
  for (const std::uint64_t seed : SeedsWithNone()) {
    INFO("chaos seed " << seed);
    for (const bool spinning : {false, true}) {
      ForkTestOptions options = Options(seed);
      // A long spin keeps the workers busy-waiting at the destructor.
      options.spin_us = spinning ? 50'000 : 0;
      jobs::ForkJoinPool pool(ForkTestConfig(7, options));
      static const jobs::ForkSiteDecl kA{.name = "test.a", .grain = 1};
      static const jobs::ForkSiteDecl kB{.name = "test.b", .grain = 1};
      jobs::ForkSite& a = pool.Site(kA);
      jobs::ForkSite& b = pool.Site(kB);
      for (int i = 0; i < 10; ++i) {
        pool.ParallelFor(a, 64, [](jobs::ChunkRange) {});
        pool.ParallelFor(b, 64, [](jobs::ChunkRange) {});
      }
      CHECK(WaitForLiveForkWorkers(before + 7));
    }
    CHECK(LiveForkWorkers() == before);
  }
}

// Inner chunks run inline on the thread of the outer chunk.
void InnerFork(const jobs::ForkSite& inner, std::atomic<int>& mismatches,
               std::atomic<int>& inner_runs) SE_REQUIRES(jobs::g_fork_chunk) {
  const int outer_thread = jobs::CurrentWorker();
  jobs::NestedParallelFor(inner, 8, [&](jobs::ChunkRange) {
    if (jobs::CurrentWorker() != outer_thread) mismatches.fetch_add(1);
    inner_runs.fetch_add(1);
  });
}

TEST_CASE("Nested forks run inline on the worker", "[jobs][pool]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  jobs::ForkJoinPool pool(ForkTestConfig(3));
  jobs::ForkSite& outer = pool.Site(TestSite());
  const jobs::ForkSite inner{.name = "test.inner", .grain = 1};
  std::atomic<int> mismatches{0};
  std::atomic<int> inner_runs{0};
  std::atomic<int> on_workers{0};
  for (int round = 0; round < 20; ++round) {
    // Four chunks that wait for each other: each runs on its own thread,
    // three of them on workers.
    std::atomic<int> arrived{0};
    pool.ParallelFor(outer, 4,
                     [&](jobs::ChunkRange) SE_REQUIRES(jobs::g_fork_chunk) {
                       arrived.fetch_add(1);
                       while (arrived.load() < 4) std::this_thread::yield();
                       if (jobs::CurrentWorker() >= 0) on_workers.fetch_add(1);
                       InnerFork(inner, mismatches, inner_runs);
                     });
  }
  REQUIRE(mismatches.load() == 0);
  REQUIRE(inner_runs.load() == 20 * 4 * 8);
  REQUIRE(on_workers.load() == 20 * 3);
  REQUIRE(inner.stats.forks == 0);
}

TEST_CASE("Per-chunk outputs concatenate in chunk order for every W",
          "[jobs][pool][property]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  constexpr std::uint32_t kCount = 777;
  // Each item emits 0 to 3 values.
  const auto emit = [](std::uint32_t i, std::vector<std::uint32_t>& out) {
    for (std::uint32_t k = 0; k < (i * 2654435761U) % 4; ++k) {
      out.push_back(i * 4 + k);
    }
  };
  std::vector<std::uint32_t> serial;
  for (std::uint32_t i = 0; i < kCount; ++i) emit(i, serial);
  for (const int workers : {0, 1, 3, 7}) {
    for (const std::uint64_t seed : SeedsWithNone()) {
      INFO("W = " << workers << ", chaos seed " << seed);
      ForkTestOptions options = Options(seed);
      options.grain = 5;
      jobs::ForkJoinPool pool(ForkTestConfig(workers, options));
      jobs::ForkSite& site = pool.Site(TestSite());
      jobs::ChunkOutputs<std::uint32_t> outputs;
      for (int round = 0; round < 5; ++round) {
        outputs.Reset(pool.ChunkCountFor(site, kCount));
        REQUIRE(outputs.Chunks() == jobs::ChunkCount(kCount, 5));
        pool.ParallelFor(site, kCount, [&](jobs::ChunkRange r) {
          std::vector<std::uint32_t>& out = outputs.At(r);
          for (std::uint32_t i = r.begin; i < r.end; ++i) emit(i, out);
        });
        std::vector<std::uint32_t> joined;
        outputs.AppendTo(joined);
        REQUIRE(joined == serial);
      }
    }
  }
}

TEST_CASE("A PerformanceOnly site uses only its workers", "[jobs][pool]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  jobs::ForkJoinPool pool(TestHostConfig({2, 2, 1, 1}, false, 1));
  static const jobs::ForkSiteDecl kOnly{
      .name = "test.performance_only",
      .grain = 1,
      .cores = jobs::CoreClass::kPerformanceOnly};
  jobs::ForkSite& only = pool.Site(kOnly);
  jobs::ForkSite& any = pool.Site(TestSite());
  REQUIRE(pool.Stats().performance_only_workers == 1);
  std::set<int> threads;
  std::vector<int> ran_on(256, -2);
  for (int round = 0; round < 20; ++round) {
    pool.ParallelFor(only, 256, [&](jobs::ChunkRange r) {
      ran_on[r.chunk] = jobs::CurrentWorker();
    });
    threads.insert(ran_on.begin(), ran_on.end());
  }
  REQUIRE(only.stats.participants == 2);
  for (const int thread : threads) REQUIRE(thread <= 0);
  pool.ParallelFor(any, 256, [](jobs::ChunkRange) {});
  REQUIRE(any.stats.participants == 5);
}

TEST_CASE("ForkStats count participants and join percentiles", "[jobs][pool]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  ForkTestOptions options;
  options.chunks_per_participant = 4;
  jobs::ForkJoinPool pool(ForkTestConfig(3, options));
  jobs::ForkSite& site = pool.Site(TestSite());
  for (int i = 0; i < 200; ++i) {
    // 12 chunks: participants = min(3 + 1, 12 / 4) = 3.
    pool.ParallelFor(site, 12, [](jobs::ChunkRange) {});
  }
  // 7 chunks give one participant: inline.
  pool.ParallelFor(site, 7, [](jobs::ChunkRange) {});
  pool.RefreshStats();
  const jobs::ForkStats& stats = site.stats;
  REQUIRE(stats.forks == 201);
  REQUIRE(stats.threaded_forks == 200);
  REQUIRE(stats.inline_forks == 1);
  CHECK(stats.chunks == 200 * 12 + 7);
  REQUIRE(stats.participants == 3);
  REQUIRE(stats.max_participants == 3);
  CHECK(stats.join_p50_us > 0.0);
  CHECK(stats.join_p99_us >= stats.join_p50_us);
  CHECK(stats.join_p999_us >= stats.join_p99_us);
  CHECK(stats.join_max_us >= stats.join_p999_us);
  REQUIRE(site.joins.Count() == 200);
  const jobs::PoolStats pool_stats = pool.Stats();
  REQUIRE(pool_stats.bumps == 400);
  REQUIRE(pool_stats.threaded_forks == 200);
}

TEST_CASE("A join slower than 4x its median runs the site inline for 5 s",
          "[jobs][pool]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  jobs::ForkJoinPool::Config config = ForkTestConfig(3);
  config.fallback_factor = 4;
  config.now_ns = &TestNow;
  jobs::ForkJoinPool pool(config);
  jobs::ForkSite& site = pool.Site(TestSite());
  const auto fork = [&](std::uint64_t join_ns) SE_REQUIRES(g_sim_owner) {
    pool.ParallelFor(site, 16, [join_ns](jobs::ChunkRange r) {
      if (r.chunk == 0) g_test_now.fetch_add(join_ns);
    });
  };
  for (int i = 0; i < 64; ++i) fork(10'000);
  REQUIRE(site.stats.threaded_forks == 64);
  REQUIRE(site.median_ns == 10'000);
  // 4x the median is not over it.
  fork(40'000);
  REQUIRE(site.stats.fallbacks == 0);
  // Over it: the window opens.
  fork(40'001);
  REQUIRE(site.stats.fallbacks == 1);
  const std::uint64_t until = site.inline_until_ns;
  CHECK(until == g_test_now.load() + 5'000'000'000);
  // Inside the window every fork runs inline (and counts).
  const std::uint64_t threaded = site.stats.threaded_forks;
  for (int i = 0; i < 10; ++i) fork(10'000);
  CHECK(site.stats.threaded_forks == threaded);
  REQUIRE(site.stats.fallback_forks == 10);
  // After 5 s it retries.
  g_test_now.store(until);
  fork(10'000);
  CHECK(site.stats.threaded_forks == threaded + 1);
  REQUIRE(site.stats.fallbacks == 1);
}

TEST_CASE("The battery poll is not counted in the join", "[jobs][pool]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  jobs::ForkJoinPool::Config config = TestHostConfig({0, 0, 0}, false, 0);
  config.fallback_factor = 4;
  config.now_ns = &TestNow;
  jobs::ForkJoinPool pool(config);
  jobs::ForkSite& site = pool.Site(TestSite());
  const auto fork = [&](std::uint64_t join_ns) SE_REQUIRES(g_sim_owner) {
    pool.ParallelFor(site, 16, [join_ns](jobs::ChunkRange r) {
      if (r.chunk == 0) g_test_now.fetch_add(join_ns);
    });
  };
  for (int i = 0; i < 64; ++i) fork(10'000);
  REQUIRE(site.stats.threaded_forks == 64);
  REQUIRE(site.median_ns == 10'000);
  // A second later the next fork polls the battery state, which takes 1 ms
  // here: 100 times the median, and none of the join.
  g_test_poll_ns.store(1'000'000);
  g_test_now.fetch_add(1'000'000'000);
  const std::uint64_t before = g_test_now.load();
  fork(10'000);
  g_test_poll_ns.store(0);
  REQUIRE(g_test_now.load() == before + 1'010'000);
  REQUIRE(site.stats.fallbacks == 0);
  CHECK(site.stats.join_max_us < 11.0);
  fork(10'000);
  REQUIRE(site.stats.threaded_forks == 66);
}

TEST_CASE("Workers spin after a join only when a fork is predicted",
          "[jobs][pool]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  static const jobs::ForkSiteDecl kA{.name = "test.first", .grain = 1};
  static const jobs::ForkSiteDecl kB{.name = "test.second", .grain = 1};
  for (const bool on_battery : {false, true}) {
    INFO("on battery " << on_battery);
    jobs::ForkJoinPool::Config config =
        TestHostConfig({0, 0, 0}, on_battery, 3);
    // The gaps between forks on the test clock: b follows a at once, a
    // follows b after 1 ms.
    config.now_ns = &TestNow;
    jobs::ForkJoinPool pool(config);
    jobs::ForkSite& a = pool.Site(kA);
    jobs::ForkSite& b = pool.Site(kB);
    // Four chunks that wait for each other, so every worker runs one and
    // then spins when the hint says so.
    const auto rendezvous =
        [&pool](jobs::ForkSite& site) SE_REQUIRES(g_sim_owner) {
          std::atomic<int> arrived{0};
          pool.ParallelFor(site, 4, [&arrived](jobs::ChunkRange) {
            arrived.fetch_add(1);
            while (arrived.load() < 4) std::this_thread::yield();
          });
        };
    // After each of a's joins but the first (a's hint is learned when b
    // follows it) the three workers spin. The test waits for them before it
    // forks b: a worker that found b's fork before it looked at the hint
    // would go on to b's chunk without spinning, which is fine for the pool
    // and would make the count below depend on the machine's load.
    const auto spun = [&pool](std::uint64_t spins) {
      const std::uint64_t start = ProfileNowNs();
      while (pool.Stats().spins < spins) {
        if (ProfileNowNs() - start > 10'000'000'000) return false;
        std::this_thread::yield();
      }
      return true;
    };
    for (int i = 0; i < 20; ++i) {
      rendezvous(a);
      if (!on_battery) REQUIRE(spun(static_cast<std::uint64_t>(3 * i)));
      rendezvous(b);
      g_test_now.fetch_add(1'000'000);
    }
    REQUIRE(a.followed_quickly);
    REQUIRE_FALSE(b.followed_quickly);
    REQUIRE(AllWakesSeen(pool));
    const jobs::PoolStats stats = pool.Stats();
    if (on_battery) {
      REQUIRE(stats.spins == 0);
    } else {
      REQUIRE(stats.spins >= std::uint64_t{3} * 19);
    }
  }
}

TEST_CASE("Each efficiency class is calibrated when the workers start",
          "[jobs][pool]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  jobs::ForkJoinPool::Config config = TestHostConfig({2, 2, 1, 1, 1}, false, 2);
  config.calibrate = true;
  jobs::ForkJoinPool pool(config);
  REQUIRE(pool.StartWorkers());
  const auto calibrations = pool.Calibrations();
  REQUIRE(calibrations.size() == 2);
  REQUIRE(calibrations[0].efficiency_class == 2);
  REQUIRE(calibrations[0].workers == 2);
  REQUIRE(calibrations[0].chunks == 400);
  REQUIRE(calibrations[1].efficiency_class == 1);
  REQUIRE(calibrations[1].workers == 3);
  REQUIRE(calibrations[1].chunks == 600);
  for (const jobs::Calibration& c : calibrations) {
    CHECK(c.chunk_p50_us > 0.0);
    // Chunks are sized for about 50 us on the caller: a median a hundred
    // times that means the sizing loop was not timed (an optimised build
    // once dropped it and sized the chunks at 0.4 s).
    CHECK(c.chunk_p50_us < 5000.0);
    CHECK(c.chunk_max_us >= c.chunk_p50_us);
    CHECK(c.wall_ms > 0.0);
    CHECK(c.relative_speed > 0.0);
  }
}

TEST_CASE("This machine's workers calibrate as one class", "[jobs][pool]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  jobs::ForkJoinPool::Config config = ForkTestConfig(-1);
  config.calibrate = true;
  jobs::ForkJoinPool pool(config);
  if (!pool.StartWorkers()) {
    SUCCEED("W_max is 0 on this machine");
    return;
  }
  const auto calibrations = pool.Calibrations();
  REQUIRE_FALSE(calibrations.empty());
  int workers = 0;
  for (const jobs::Calibration& c : calibrations) {
    workers += c.workers;
    CHECK(c.chunk_p50_us > 0.0);
    CHECK(c.chunk_p50_us < 5000.0);
  }
  CHECK(workers == pool.ActiveWorkers());
}

}  // namespace
}  // namespace se
