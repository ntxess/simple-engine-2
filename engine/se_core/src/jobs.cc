// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/jobs.h"

namespace se::jobs {

ForkJoinPool::ForkJoinPool() noexcept = default;

ForkJoinPool::ForkJoinPool(const Config& config) noexcept : config_(config) {}

}  // namespace se::jobs
