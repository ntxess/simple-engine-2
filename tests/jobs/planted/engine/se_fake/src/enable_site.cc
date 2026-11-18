// Copyright 2026 The simple-engine-2 Authors.
//
// A planted violation for checks.planted.fork_site_enabled (never
// compiled): shipped code that enables a fork site.

#include "se_core/jobs.h"

namespace se {

inline void PlantedEnable(jobs::ForkJoinPool::Config& config) {
  config.enabled_sites = {"rampage.Path"};
}

}  // namespace se
