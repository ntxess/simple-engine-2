// Copyright 2026 The simple-engine-2 Authors.
//
// Counts calls to the global operator new in se_render_tests, which replaces
// it (alloc_counter.cc). Sanitizer builds keep their runtime's allocator, so
// there the count is unavailable and tests fall back to buffer checks.

#ifndef TESTS_RENDER_ALLOC_COUNTER_H_
#define TESTS_RENDER_ALLOC_COUNTER_H_

#include <cstdint>

namespace se::testing {

// Whether AllocationCount() counts (false in sanitizer builds).
[[nodiscard]] bool CountingAllocations() noexcept;

// Global operator new calls (every non-aligned form) since the process
// started.
[[nodiscard]] std::uint64_t AllocationCount() noexcept;

}  // namespace se::testing

#endif  // TESTS_RENDER_ALLOC_COUNTER_H_
