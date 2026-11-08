// Copyright 2026 The simple-engine-2 Authors.
//
// Compile-fail test (clang presets). A fork-chunk lambda that calls
// World::Create() must not compile under -Werror=thread-safety: the lambda
// body does not hold g_sim_owner, so the analysis rejects the structural
// call. With SE_COMPILE_FAIL=0 the same code makes the call on the owner's
// side of the fork and compiles, which shows the failure comes from the
// lambda alone.

#include "se_core/guid.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"

namespace se {

void SpawnFromFork(World& world, jobs::ForkJoinPool& pool, jobs::ForkSite& site)
    SE_REQUIRES(g_sim_owner) {
#if SE_COMPILE_FAIL
  pool.ParallelFor(site, 4, [&](jobs::ChunkRange range) {
    static_cast<void>(world.Create(Guid{1, range.chunk + 1ULL}));
  });
#else
  pool.ParallelFor(site, 4, [](jobs::ChunkRange) {});
  static_cast<void>(world.Create(Guid{1, 1}));
#endif
}

}  // namespace se
