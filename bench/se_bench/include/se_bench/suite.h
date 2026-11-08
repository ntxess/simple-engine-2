// Copyright 2026 The simple-engine-2 Authors.
//
// se_bench suites (docs/milestone-specs.md M3c). A suite is a function that
// builds its scene, then hands the step it measures to a Sampler, which runs
// it untimed for the warm-up and times every following iteration:
//
//   std::expected<void, Error> RunMySuite(const SuiteOptions& options,
//                                         Sampler& sampler) {
//     ... set up options.n items ...
//     sampler.Run([&] { Step(); });
//     sampler.AddCounter("items", static_cast<double>(options.n));
//     return {};
//   }
//   const SuiteRegistrar kMySuite(
//       {.name = "my_suite", ..., .run = &RunMySuite});
//
// Adding a suite is one source file holding the function and its registrar
// (the registrar runs at start-up, so nothing else names the suite) plus one
// se_bench_suite() line in bench/se_bench/CMakeLists.txt, which links the
// module the suite measures and registers the CTest gate bench.<name>, and
// the suite's entry (scene size, iterations, p95 limit) in
// bench/thresholds.json. Suite files are compiled into se_bench itself,
// never into a static library, where the linker could drop the
// unreferenced registrar.

#ifndef SE_BENCH_SUITE_H_
#define SE_BENCH_SUITE_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"
#include "se_core/profile_clock.h"

namespace se::bench {

struct SuiteOptions {
  // The scene size (items, sprites, entities: the suite says).
  std::uint32_t n = 0;
  // Timed iterations, after |warmup| untimed ones.
  std::uint32_t iterations = 0;
  std::uint32_t warmup = 0;
  std::uint64_t seed = 1;
};

// A number a suite reports beside the timings (quads drawn, contacts, ...).
struct Counter {
  std::string name;
  double value = 0.0;
};

class Sampler {
 public:
  explicit Sampler(const SuiteOptions& options) : options_(options) {}

  // Runs |step| options.warmup times untimed, then options.iterations times,
  // recording each call's duration. A suite calls it once.
  template <class F>
  void Run(const F& step) {
    for (std::uint32_t i = 0; i < options_.warmup; ++i) step();
    samples_ns_.clear();
    samples_ns_.reserve(options_.iterations);
    for (std::uint32_t i = 0; i < options_.iterations; ++i) {
      const std::uint64_t start = ProfileNowNs();
      step();
      samples_ns_.push_back(ProfileNowNs() - start);
    }
  }

  void AddCounter(std::string_view name, double value) {
    counters_.push_back({std::string(name), value});
  }

  [[nodiscard]] std::span<const std::uint64_t> SamplesNs() const noexcept {
    return samples_ns_;
  }
  [[nodiscard]] std::span<const Counter> Counters() const noexcept {
    return counters_;
  }

 private:
  SuiteOptions options_;
  std::vector<std::uint64_t> samples_ns_;
  std::vector<Counter> counters_;
};

using SuiteFn = std::expected<void, Error> (*)(const SuiteOptions& options,
                                               Sampler& sampler);

struct SuiteDef {
  std::string_view name;
  std::string_view description;
  // Used when the command line does not set them.
  std::uint32_t default_n = 0;
  std::uint32_t default_iterations = 0;
  std::uint32_t default_warmup = 0;
  SuiteFn run = nullptr;
};

// Registers a suite during static initialisation. Define one per suite at
// namespace scope in the suite's source file.
class SuiteRegistrar {
 public:
  explicit SuiteRegistrar(const SuiteDef& def);
};

// Every registered suite, sorted by name.
[[nodiscard]] std::vector<SuiteDef> Suites();
[[nodiscard]] const SuiteDef* FindSuite(std::string_view name);

// Nearest-rank statistics of a set of samples (the median is the 50th
// percentile, so it is always one of the samples).
struct Summary {
  std::size_t count = 0;
  std::uint64_t min_ns = 0;
  std::uint64_t median_ns = 0;
  std::uint64_t p95_ns = 0;
  std::uint64_t p99_ns = 0;
  std::uint64_t max_ns = 0;
  double mean_ns = 0.0;
};

[[nodiscard]] Summary Summarize(std::span<const std::uint64_t> samples_ns);

}  // namespace se::bench

#endif  // SE_BENCH_SUITE_H_
