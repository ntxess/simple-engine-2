// Copyright 2026 The simple-engine-2 Authors.
// se-fault-boundary: a threaded fork's chunks (ADR-006, ADR-107). The first
// exception is rethrown on the fork's caller after the join.

#include "src/fork_fault_boundary.h"

#include <exception>

#include "se_core/jobs.h"

namespace se::jobs::internal {

bool RunChunkContained(ChunkInvoke invoke, const void* body, ChunkRange range,
                       std::exception_ptr& error) noexcept {
  try {
    invoke(body, range);
    return true;
  } catch (...) {
    error = std::current_exception();
    return false;
  }
}

}  // namespace se::jobs::internal
