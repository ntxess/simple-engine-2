// Copyright 2026 The simple-engine-2 Authors.
//
// SpscRing<T>: a bounded single-producer, single-consumer ring of trivially
// copyable values. The capacity is a power of two fixed at construction;
// nothing is allocated afterwards. TryPush fails when the ring is full and
// the caller counts the drop (a producer never waits). The producer's head
// and the consumer's tail live on their own cache lines, each next to the
// other side's index as last seen, so a push or pop touches the shared line
// only when its cached view says the ring is full or empty.
//
// One thread pushes and one thread pops; which threads they are may change
// only across a synchronisation that orders the handover (a join, a mutex).

#ifndef SE_CORE_SPSC_RING_H_
#define SE_CORE_SPSC_RING_H_

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "se_core/assert.h"

namespace se {

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324)  // padded to the cache line on purpose
#endif

template <class T>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>,
                "SpscRing copies values with plain stores");
  static_assert(std::is_default_constructible_v<T>);

 public:
  // |capacity| must be a power of two (it is rounded up to one otherwise).
  explicit SpscRing(std::uint32_t capacity)
      : capacity_(std::bit_ceil(capacity < 1 ? 1U : capacity)),
        mask_(capacity_ - 1),
        slots_(std::make_unique<T[]>(capacity_)) {
    SE_ASSERT_MSG(std::has_single_bit(capacity),
                  "SpscRing capacity must be a power of two");
  }

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer: appends |value|, or returns false when the ring is full.
  bool TryPush(const T& value) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head - producer_tail_ >= capacity_) {
      producer_tail_ = tail_.load(std::memory_order_acquire);
      if (head - producer_tail_ >= capacity_) return false;
    }
    slots_[head & mask_] = value;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer: removes the oldest value, or returns nullopt when empty.
  std::optional<T> TryPop() noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == consumer_head_) {
      consumer_head_ = head_.load(std::memory_order_acquire);
      if (tail == consumer_head_) return std::nullopt;
    }
    std::optional<T> value(slots_[tail & mask_]);
    tail_.store(tail + 1, std::memory_order_release);
    return value;
  }

  // Consumer: calls f(const T&) for every value present, oldest first, and
  // removes them. Returns how many.
  template <class F>
  std::size_t PopAll(F&& f) {
    auto&& visit = std::forward<F>(f);
    std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    consumer_head_ = head_.load(std::memory_order_acquire);
    const auto count = static_cast<std::size_t>(consumer_head_ - tail);
    for (; tail != consumer_head_; ++tail) {
      visit(static_cast<const T&>(slots_[tail & mask_]));
    }
    tail_.store(tail, std::memory_order_release);
    return count;
  }

  [[nodiscard]] std::uint32_t Capacity() const noexcept { return capacity_; }

  // Either side: values pushed and not yet popped (a snapshot).
  [[nodiscard]] std::size_t SizeApprox() const noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    return head >= tail ? static_cast<std::size_t>(head - tail) : 0;
  }

 private:
  const std::uint32_t capacity_;
  const std::uint32_t mask_;
  const std::unique_ptr<T[]> slots_;
  // Producer line: its index and the consumer's index as it last saw it.
  alignas(64) std::atomic<std::uint64_t> head_{0};
  std::uint64_t producer_tail_ = 0;
  // Consumer line.
  alignas(64) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t consumer_head_ = 0;
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

}  // namespace se

#endif  // SE_CORE_SPSC_RING_H_
