// Copyright 2026 The simple-engine-2 Authors.

#include "se_bench/suite.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace se::bench {
namespace {

// Registrars run during static initialisation, in no particular order, so
// the registry is created on first use and never destroyed.
std::vector<SuiteDef>& Registry() {
  static auto* registry = new std::vector<SuiteDef>();
  return *registry;
}

// The sample of nearest rank ceil(p * n) in a sorted set.
std::uint64_t NearestRank(std::span<const std::uint64_t> sorted, double p) {
  const auto n = static_cast<double>(sorted.size());
  const auto rank = static_cast<std::size_t>(std::ceil(p * n));
  return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

}  // namespace

SuiteRegistrar::SuiteRegistrar(const SuiteDef& def) {
  Registry().push_back(def);
}

std::vector<SuiteDef> Suites() {
  std::vector<SuiteDef> suites = Registry();
  std::sort(
      suites.begin(), suites.end(),
      [](const SuiteDef& a, const SuiteDef& b) { return a.name < b.name; });
  return suites;
}

const SuiteDef* FindSuite(std::string_view name) {
  for (const SuiteDef& def : Registry()) {
    if (def.name == name) return &def;
  }
  return nullptr;
}

Summary Summarize(std::span<const std::uint64_t> samples_ns) {
  Summary summary;
  summary.count = samples_ns.size();
  if (samples_ns.empty()) return summary;
  std::vector<std::uint64_t> sorted(samples_ns.begin(), samples_ns.end());
  std::sort(sorted.begin(), sorted.end());
  summary.min_ns = sorted.front();
  summary.max_ns = sorted.back();
  summary.median_ns = NearestRank(sorted, 0.50);
  summary.p95_ns = NearestRank(sorted, 0.95);
  summary.p99_ns = NearestRank(sorted, 0.99);
  double total = 0.0;
  for (const std::uint64_t ns : sorted) total += static_cast<double>(ns);
  summary.mean_ns = total / static_cast<double>(sorted.size());
  return summary;
}

}  // namespace se::bench
