// Copyright 2026 The simple-engine-2 Authors.
//
// Compile-fail test (clang presets). A fork-chunk lambda that forks again
// must declare that it runs under g_fork_chunk (ADR-019): without
// SE_REQUIRES(jobs::g_fork_chunk) the analysis rejects the
// NestedParallelFor call under -Werror=thread-safety. With SE_COMPILE_FAIL=0
// the declared lambda compiles, so nested forks need no opt-out of the
// analysis.

#include "se_core/jobs.h"
#include "se_core/thread_role.h"

namespace se {

void ForkTwice(jobs::ForkJoinPool& pool, jobs::ForkSite& outer,
               const jobs::ForkSite& inner) SE_REQUIRES(g_sim_owner) {
#if SE_COMPILE_FAIL
  pool.ParallelFor(outer, 4, [&](jobs::ChunkRange) {
    jobs::NestedParallelFor(inner, 2, [](jobs::ChunkRange) {});
  });
#else
  pool.ParallelFor(outer, 4,
                   [&](jobs::ChunkRange) SE_REQUIRES(jobs::g_fork_chunk) {
                     jobs::NestedParallelFor(inner, 2, [](jobs::ChunkRange) {});
                   });
#endif
}

}  // namespace se
