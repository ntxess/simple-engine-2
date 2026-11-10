// Copyright 2026 The simple-engine-2 Authors.
//
// SpscQueue<T>: a bounded single-producer, single-consumer ring of move-only
// values (docs/design/dedicated-driver.md "Synchronization", item 1), for
// channels whose entries own memory, such as the sim command ring. Where T is
// trivially copyable, SpscRing (se_core/spsc_ring.h) is the lighter choice;
// this one adds a peek (Front) for consumers that take only some entries.
//
// The capacity is a power of two fixed at construction, and nothing is
// allocated afterwards. A producer never waits: TryPush returns false when
// the ring is full and leaves the value with the caller, who coalesces,
// defers or counts the drop. Memory orders, as the design states them:
//   - push: construct the value in its cell, then head.store(release);
//   - pop: head.load(acquire), read and destroy the value, then
//     tail.store(release);
//   - the producer checks for space with tail.load(acquire).
// Each side keeps the other side's index as it last saw it on its own cache
// line, so a push or pop reads the shared line only when that copy says the
// ring is full or empty.
//
// One thread pushes and one thread pops; the threads may change only across
// a synchronisation that orders the handover (a join, a thread start).

#ifndef SE_CORE_SPSC_QUEUE_H_
#define SE_CORE_SPSC_QUEUE_H_

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
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
class SpscQueue {
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "SpscQueue moves values in and out without a fault path");
  static_assert(std::is_nothrow_destructible_v<T>);

 public:
  // |capacity| must be a power of two (it is rounded up to one otherwise).
  explicit SpscQueue(std::uint32_t capacity)
      : capacity_(std::bit_ceil(capacity < 1 ? 1U : capacity)),
        mask_(capacity_ - 1),
        cells_(std::make_unique<Cell[]>(capacity_)) {
    SE_ASSERT_MSG(std::has_single_bit(capacity),
                  "SpscQueue capacity must be a power of two");
  }

  // Destroys the values still queued. Neither side may use the queue then.
  ~SpscQueue() {
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    for (std::uint64_t t = tail_.load(std::memory_order_relaxed); t != head;
         ++t) {
      Value(t)->~T();
    }
  }

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // Producer: moves |value| in, or returns false (and leaves it untouched)
  // when the queue is full.
  bool TryPush(T&& value) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head - producer_tail_ >= capacity_) {
      producer_tail_ = tail_.load(std::memory_order_acquire);
      if (head - producer_tail_ >= capacity_) return false;
    }
    ::new (static_cast<void*>(cells_[head & mask_].bytes)) T(std::move(value));
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Producer: copies |value| in (copyable T), or returns false when full.
  template <class U = T>
    requires std::is_nothrow_copy_constructible_v<U>
  bool TryPush(const T& value) noexcept {
    T value_copy(value);
    return TryPush(std::move(value_copy));
  }

  // Consumer: the oldest value, or nullptr when the queue is empty. The
  // value stays valid until Pop().
  [[nodiscard]] T* Front() noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == consumer_head_) {
      consumer_head_ = head_.load(std::memory_order_acquire);
      if (tail == consumer_head_) return nullptr;
    }
    return Value(tail);
  }

  // Consumer: destroys the oldest value; the queue must not be empty.
  void Pop() noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == consumer_head_) {
      consumer_head_ = head_.load(std::memory_order_acquire);
    }
    SE_ASSERT_MSG(tail != consumer_head_, "SpscQueue::Pop on an empty queue");
    Value(tail)->~T();
    tail_.store(tail + 1, std::memory_order_release);
  }

  // Consumer: removes and returns the oldest value, or nullopt.
  std::optional<T> TryPop() noexcept {
    T* front = Front();
    if (front == nullptr) return std::nullopt;
    std::optional<T> value(std::move(*front));
    Pop();
    return value;
  }

  [[nodiscard]] std::uint32_t Capacity() const noexcept { return capacity_; }

  // Either side: values pushed and not yet popped (a snapshot).
  [[nodiscard]] std::size_t SizeApprox() const noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    return head >= tail ? static_cast<std::size_t>(head - tail) : 0;
  }

 private:
  struct Cell {
    alignas(T) std::byte bytes[sizeof(T)];
  };

  T* Value(std::uint64_t index) noexcept {
    return std::launder(reinterpret_cast<T*>(cells_[index & mask_].bytes));
  }

  const std::uint32_t capacity_;
  const std::uint32_t mask_;
  const std::unique_ptr<Cell[]> cells_;
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

#endif  // SE_CORE_SPSC_QUEUE_H_
