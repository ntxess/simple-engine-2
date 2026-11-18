// Copyright 2026 The simple-engine-2 Authors.
//
// The threaded ForkJoinPool's overhead and join tails (docs/milestone-specs.md
// M14, Bench; docs/design/mt-design.md "Measurements", fork-join overhead
// and tails). Each iteration is one ParallelFor of n chunks on a pool of W
// workers (se_os ForkWorkers on this machine) with its site enabled and the
// fallback rule off, so every fork is threaded; n defaults to 4 (W + 1),
// which makes participants = W + 1.
//
//   fork_empty_w<W>    empty chunks, forks back to back: the bare protocol
//                      with the workers spinning between forks (the spin
//                      prediction sees the next fork within 20 us)
//   fork_parked_w<W>   empty chunks, 2 ms between forks (untimed) on a
//                      precise timer, so the workers park and each fork
//                      wakes them
//   fork_50us_w<W>     chunks of 50 us of busy work, parked between forks:
//                      ideal = n * 50 us / (W + 1); the overhead counter is
//                      the median minus that
//
// Counters: the site's ForkStats (participants, join p50/p99/p99.9/max,
// missed and late wakes), the pool's wakes (the largest wake latency) and
// spins, and the median of the same fork run inline.

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "se_bench/suite.h"
#include "se_core/error.h"
#include "se_core/jobs.h"
#include "se_core/profile_clock.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_os/fork_workers.h"
#include "se_os/precise_wait.h"

namespace se::bench {
namespace {

struct ForkBench {
  int workers = 1;
  std::uint64_t chunk_ns = 0;
  bool parked = false;
};

constexpr std::uint64_t kParkNs = 2'000'000;

void BusyFor(std::uint64_t ns) {
  if (ns == 0) return;
  const std::uint64_t start = ProfileNowNs();
  while (ProfileNowNs() - start < ns) {
  }
}

const jobs::ForkSiteDecl& BenchSite() {
  static const jobs::ForkSiteDecl kSite{.name = "bench.fork", .grain = 1};
  return kSite;
}

std::expected<void, Error> RunForkBench(const SuiteOptions& options,
                                        Sampler& sampler,
                                        const ForkBench& bench) {
  AssertMainThread();
  jobs::ForkJoinPool::Config config;
  config.workers = bench.workers;
  config.threads = ForkWorkersFactory();
  config.enabled_sites = {std::string(BenchSite().name)};
  // Every fork threaded: a slow join must not send the site inline.
  config.fallback_factor = 0;
  jobs::ForkJoinPool pool(config);
  jobs::ForkSite* site = nullptr;
  {
    const InlineOwnerScope owner(OnMainThread());
    site = &pool.Site(BenchSite());
  }
  if (pool.ActiveWorkers() != bench.workers) {
    return Fail(ErrorCode::kUnsupported,
                std::format("{} fork workers did not start ({} did)",
                            bench.workers, pool.ActiveWorkers()));
  }
  const std::uint32_t chunks = options.n;
  const std::uint64_t chunk_ns = bench.chunk_ns;
  // Like a tick, each fork takes the World ownership on the main thread
  // (two thread-id checks, nanoseconds against the fork).
  const auto fork = [&pool, site, chunks, chunk_ns] {
    AssertMainThread();
    const InlineOwnerScope owner(OnMainThread());
    pool.ParallelFor(*site, chunks,
                     [chunk_ns](jobs::ChunkRange) { BusyFor(chunk_ns); });
  };

  // The same fork inline, for comparison.
  Summary inline_summary;
  {
    const InlineOwnerScope owner(OnMainThread());
    jobs::ForkJoinPool serial;
    jobs::ForkSite inline_site{.name = "bench.fork_inline", .grain = 1};
    std::vector<std::uint64_t> samples;
    const std::uint32_t runs = std::min<std::uint32_t>(options.iterations, 50);
    for (std::uint32_t i = 0; i < runs; ++i) {
      const std::uint64_t start = ProfileNowNs();
      serial.ParallelFor(inline_site, chunks,
                         [chunk_ns](jobs::ChunkRange) { BusyFor(chunk_ns); });
      samples.push_back(ProfileNowNs() - start);
    }
    inline_summary = Summarize(samples);
  }

  if (bench.parked) {
    auto timer = PreciseTimer::Create();
    if (!timer) return std::unexpected(std::move(timer.error()));
    sampler.Run(fork,
                [&timer] { timer->SleepUntil(MonotonicNowNs() + kParkNs); });
  } else {
    sampler.Run(fork);
  }

  {
    const InlineOwnerScope owner(OnMainThread());
    pool.RefreshStats();
  }
  const Summary summary = Summarize(sampler.SamplesNs());
  const jobs::ForkStats& stats = site->stats;
  const jobs::PoolStats pool_stats = pool.Stats();
  const double ideal_us = static_cast<double>(chunks) *
                          static_cast<double>(chunk_ns) * 1e-3 /
                          std::max<std::uint32_t>(stats.participants, 1);
  sampler.AddCounter("workers", bench.workers);
  sampler.AddCounter("chunks", chunks);
  sampler.AddCounter("participants", stats.participants);
  sampler.AddCounter("threaded_forks",
                     static_cast<double>(stats.threaded_forks));
  sampler.AddCounter("inline_forks", static_cast<double>(stats.inline_forks));
  sampler.AddCounter("join_p50_us", stats.join_p50_us);
  sampler.AddCounter("join_p99_us", stats.join_p99_us);
  sampler.AddCounter("join_p999_us", stats.join_p999_us);
  sampler.AddCounter("join_max_us", stats.join_max_us);
  sampler.AddCounter("missed_wakes", static_cast<double>(stats.missed_wakes));
  sampler.AddCounter("late_wakes", static_cast<double>(stats.late_wakes));
  sampler.AddCounter("wakes", static_cast<double>(pool_stats.wakes));
  sampler.AddCounter(
      "wake_latency_max_us",
      static_cast<double>(pool_stats.wake_latency_max_ns) * 1e-3);
  sampler.AddCounter("spins", static_cast<double>(pool_stats.spins));
  sampler.AddCounter("spin_hits", static_cast<double>(pool_stats.spin_hits));
  sampler.AddCounter("ideal_us", ideal_us);
  sampler.AddCounter("overhead_p50_us",
                     static_cast<double>(summary.median_ns) * 1e-3 - ideal_us);
  sampler.AddCounter("inline_median_us",
                     static_cast<double>(inline_summary.median_ns) * 1e-3);
  for (const jobs::Calibration& c : pool.Calibrations()) {
    sampler.AddCounter(
        std::format("calibration_class{}_chunk_p50_us", c.efficiency_class),
        c.chunk_p50_us);
  }
  return {};
}

template <int Workers, std::uint64_t ChunkNs, bool Parked>
std::expected<void, Error> RunSuite(const SuiteOptions& options,
                                    Sampler& sampler) {
  return RunForkBench(
      options, sampler,
      ForkBench{.workers = Workers, .chunk_ns = ChunkNs, .parked = Parked});
}

constexpr std::uint32_t DefaultChunks(int workers) {
  return 4 * static_cast<std::uint32_t>(workers + 1);
}

const SuiteRegistrar kForkEmptyW1({
    .name = "fork_empty_w1",
    .description = "Empty fork back to back, W = 1",
    .default_n = DefaultChunks(1),
    .default_iterations = 5000,
    .default_warmup = 200,
    .run = &RunSuite<1, 0, false>,
});
const SuiteRegistrar kForkEmptyW3({
    .name = "fork_empty_w3",
    .description = "Empty fork back to back, W = 3",
    .default_n = DefaultChunks(3),
    .default_iterations = 5000,
    .default_warmup = 200,
    .run = &RunSuite<3, 0, false>,
});
const SuiteRegistrar kForkEmptyW7({
    .name = "fork_empty_w7",
    .description = "Empty fork back to back, W = 7",
    .default_n = DefaultChunks(7),
    .default_iterations = 5000,
    .default_warmup = 200,
    .run = &RunSuite<7, 0, false>,
});
const SuiteRegistrar kForkParkedW1({
    .name = "fork_parked_w1",
    .description = "Empty fork waking parked workers, W = 1",
    .default_n = DefaultChunks(1),
    .default_iterations = 1000,
    .default_warmup = 50,
    .run = &RunSuite<1, 0, true>,
});
const SuiteRegistrar kForkParkedW3({
    .name = "fork_parked_w3",
    .description = "Empty fork waking parked workers, W = 3",
    .default_n = DefaultChunks(3),
    .default_iterations = 1000,
    .default_warmup = 50,
    .run = &RunSuite<3, 0, true>,
});
const SuiteRegistrar kForkParkedW7({
    .name = "fork_parked_w7",
    .description = "Empty fork waking parked workers, W = 7",
    .default_n = DefaultChunks(7),
    .default_iterations = 1000,
    .default_warmup = 50,
    .run = &RunSuite<7, 0, true>,
});
const SuiteRegistrar kFork50usW1({
    .name = "fork_50us_w1",
    .description = "Fork of 50 us chunks waking parked workers, W = 1",
    .default_n = DefaultChunks(1),
    .default_iterations = 1000,
    .default_warmup = 50,
    .run = &RunSuite<1, 50'000, true>,
});
const SuiteRegistrar kFork50usW3({
    .name = "fork_50us_w3",
    .description = "Fork of 50 us chunks waking parked workers, W = 3",
    .default_n = DefaultChunks(3),
    .default_iterations = 1000,
    .default_warmup = 50,
    .run = &RunSuite<3, 50'000, true>,
});
const SuiteRegistrar kFork50usW7({
    .name = "fork_50us_w7",
    .description = "Fork of 50 us chunks waking parked workers, W = 7",
    .default_n = DefaultChunks(7),
    .default_iterations = 1000,
    .default_warmup = 50,
    .run = &RunSuite<7, 50'000, true>,
});

}  // namespace
}  // namespace se::bench
