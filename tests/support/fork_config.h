// Copyright 2026 The simple-engine-2 Authors.
//
// ForkJoinPool configurations for the M14 tests: W workers on se_os's
// ForkWorkers (role kForkWorker), every site enabled, no fallback window (a
// slow join in a busy test would otherwise send the site inline for 5 s)
// and no calibration fork. W = 0 builds the same configuration without
// workers, so every W of a comparison chunks alike. The chaos seed is the
// argument, or SE_CHAOS_SEED when the argument is 0.

#ifndef TESTS_SUPPORT_FORK_CONFIG_H_
#define TESTS_SUPPORT_FORK_CONFIG_H_

#include <array>
#include <cstdint>

#include "se_core/jobs.h"

namespace se::testing {

// The worker counts and chaos seeds of the determinism matrix
// (docs/milestone-specs.md M14: W = 0/1/3/7, 8 chaos seeds).
inline constexpr std::array<int, 4> kForkWorkerCounts = {0, 1, 3, 7};
inline constexpr std::array<std::uint64_t, 8> kChaosSeeds = {
    0x1, 0x2F, 0x3A7, 0x51C3, 0x6B0D1, 0x7EE5F, 0x9C4B2D, 0xD15EA5E};

struct ForkTestOptions {
  // Every site's grain (0 = the sites' own): small scenes still fork.
  std::uint32_t grain = 0;
  std::uint32_t chunks_per_participant = 1;
  // 0 = SE_CHAOS_SEED (or none).
  std::uint64_t chaos_seed = 0;
  std::uint32_t spin_us = 20;
};

[[nodiscard]] jobs::ForkJoinPool::Config ForkTestConfig(
    int workers, const ForkTestOptions& options = {});

}  // namespace se::testing

#endif  // TESTS_SUPPORT_FORK_CONFIG_H_
