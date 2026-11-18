// Copyright 2026 The simple-engine-2 Authors.
//
// Replaces the global operator new and delete of se_render_tests with
// malloc and free plus a counter. Sanitizers bring their own operator new
// (and check that new pairs with delete), so SE_COUNT_ALLOCATIONS is 0 in
// those builds and nothing is replaced.

#include "render/alloc_counter.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace se::testing {
namespace {

std::atomic<std::uint64_t> g_allocations{0};

}  // namespace

bool CountingAllocations() noexcept { return SE_COUNT_ALLOCATIONS != 0; }

std::uint64_t AllocationCount() noexcept {
  return g_allocations.load(std::memory_order_relaxed);
}

#if SE_COUNT_ALLOCATIONS
namespace {

// Counts one allocation and makes it with malloc (the replaced operators
// below call it).
void* CountedAlloc(std::size_t size) noexcept {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(size == 0 ? 1 : size);
}

}  // namespace
#endif  // SE_COUNT_ALLOCATIONS

}  // namespace se::testing

#if SE_COUNT_ALLOCATIONS
void* operator new(std::size_t size) {
  void* p = se::testing::CountedAlloc(size);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t size) {
  void* p = se::testing::CountedAlloc(size);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  return se::testing::CountedAlloc(size);
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  return se::testing::CountedAlloc(size);
}
// The CRT declares the deletes with the parameter name _Block, which is
// reserved for the implementation, so the definitions cannot repeat it.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* p) noexcept { std::free(p); }
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete[](void* p) noexcept { std::free(p); }
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* p, std::size_t /*size*/) noexcept { std::free(p); }
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete[](void* p, std::size_t /*size*/) noexcept { std::free(p); }
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* p, const std::nothrow_t& /*tag*/) noexcept {
  std::free(p);
}
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete[](void* p, const std::nothrow_t& /*tag*/) noexcept {
  std::free(p);
}
#endif  // SE_COUNT_ALLOCATIONS
