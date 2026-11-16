// Copyright 2026 The simple-engine-2 Authors.

#include "support/fork_config.h"

#include <cstdint>

#include "se_core/jobs.h"
#include "se_os/fork_workers.h"

namespace se::testing {

jobs::ForkJoinPool::Config ForkTestConfig(int workers,
                                          const ForkTestOptions& options) {
  jobs::ForkJoinPool::Config config;
  config.workers = workers;
  config.threads = ForkWorkersFactory();
  config.enabled_sites = {"*"};
  config.fallback_factor = 0;
  config.calibrate = false;
  config.grain_override = options.grain;
  config.chunks_per_participant = options.chunks_per_participant;
  config.spin_us = options.spin_us;
  config.chaos_seed =
      options.chaos_seed != 0 ? options.chaos_seed : ChaosSeedFromEnv();
  return config;
}

}  // namespace se::testing
