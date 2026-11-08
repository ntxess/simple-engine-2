// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/jobs.h"

namespace se::jobs {

ForkChunkRole g_fork_chunk;

// g_fork_chunk is a static token with nothing to lock, so acquiring and
// releasing it are excluded from the analysis (as for InlineOwnerScope).
ForkScope::ForkScope() noexcept SE_NO_THREAD_SAFETY_ANALYSIS {
  t_in_fork = true;
}

ForkScope::~ForkScope() SE_NO_THREAD_SAFETY_ANALYSIS { t_in_fork = previous_; }

ForkJoinPool::ForkJoinPool() noexcept = default;

ForkJoinPool::ForkJoinPool(const Config& config) noexcept : config_(config) {}

}  // namespace se::jobs
