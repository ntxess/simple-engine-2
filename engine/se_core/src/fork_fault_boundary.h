// Copyright 2026 The simple-engine-2 Authors.
//
// The threaded ForkJoinPool's fault boundary (internal to se_core): every
// chunk a worker or the caller runs during a threaded fork goes through
// RunChunkContained, which catches what the chunk throws, so the caller can
// rethrow it after the join instead of unwinding while other threads still
// read the fork's descriptor (ADR-006, ADR-107).

#ifndef ENGINE_SE_CORE_SRC_FORK_FAULT_BOUNDARY_H_
#define ENGINE_SE_CORE_SRC_FORK_FAULT_BOUNDARY_H_

#include <exception>

#include "se_core/jobs.h"
#include "se_core/thread_safety.h"

namespace se::jobs::internal {

using ChunkInvoke = void (*)(const void* body, ChunkRange range);

// Runs invoke(body, range). Returns false, with the exception in |error|,
// when the chunk threw.
bool RunChunkContained(ChunkInvoke invoke, const void* body, ChunkRange range,
                       std::exception_ptr& error) noexcept
    SE_REQUIRES(g_fork_chunk);

}  // namespace se::jobs::internal

#endif  // ENGINE_SE_CORE_SRC_FORK_FAULT_BOUNDARY_H_
