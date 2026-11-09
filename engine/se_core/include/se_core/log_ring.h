// Copyright 2026 The simple-engine-2 Authors.
//
// LogRing: one thread's ring of LogRecords, laid out as a kind-2 dump stream
// (se_core/dump_stream.h): the header, then the records, in one allocation.
// header.head is the producer index and header.dropped the drop count, so a
// crash dump reads the live state. The producer is the thread the ring
// belongs to (the shared library ring serialises its producers with a
// mutex); the consumer is the log worker. A full ring drops the new record
// and counts it; a producer never waits. Records stay in the ring's memory
// after the worker has consumed them, so the newest `capacity` records are
// the crash tail.
//
// Capacities: 4096 records on the main thread, 1024 on every other thread
// and in the shared library ring.

#ifndef SE_CORE_LOG_RING_H_
#define SE_CORE_LOG_RING_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "se_core/dump_stream.h"
#include "se_core/thread_role.h"

namespace se {

inline constexpr std::uint32_t kMainLogRingCapacity = 4096;
inline constexpr std::uint32_t kThreadLogRingCapacity = 1024;
inline constexpr std::uint32_t kLibraryLogRingCapacity = 1024;

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324)  // padded to the cache line on purpose
#endif

class LogRing {
 public:
  // Allocates a ring of |capacity| records (a power of two), or returns
  // nullptr when the memory is not available.
  [[nodiscard]] static std::unique_ptr<LogRing> Create(
      std::uint32_t capacity, std::string_view name, ThreadRole role,
      std::uint64_t os_thread_id) noexcept;

  ~LogRing();
  LogRing(const LogRing&) = delete;
  LogRing& operator=(const LogRing&) = delete;

  // Producer: appends |record|, or counts a drop and returns false when the
  // ring is full.
  bool TryPush(const LogRecord& record) noexcept;

  // Counts a record that could not be pushed (any thread).
  void CountDrop() noexcept;

  // Consumer: calls f(const LogRecord&) for every record not yet consumed,
  // oldest first. Returns how many.
  template <class F>
  std::uint64_t Drain(F&& f) {
    auto&& visit = std::forward<F>(f);
    const std::uint64_t head =
        AtomicLoad(header_->head, std::memory_order_acquire);
    std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t count = head - tail;
    for (; tail != head; ++tail) {
      visit(static_cast<const LogRecord&>(records_[tail & mask_]));
    }
    tail_.store(head, std::memory_order_release);
    return count;
  }

  // Consumer: true when records wait.
  [[nodiscard]] bool HasRecords() const noexcept {
    return AtomicLoad(header_->head, std::memory_order_acquire) !=
           tail_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t Head() const noexcept {
    return AtomicLoad(header_->head, std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t Dropped() const noexcept {
    return AtomicLoad(header_->dropped, std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint32_t Capacity() const noexcept { return capacity_; }

  // The producer's name and role, as the header shows them.
  [[nodiscard]] std::array<char, kDumpStreamNameSize + 1> Name()
      const noexcept {
    return LoadDumpName(header_->name);
  }
  [[nodiscard]] ThreadRole Role() const noexcept {
    return static_cast<ThreadRole>(
        AtomicLoad(header_->role, std::memory_order_relaxed));
  }

  // Records a new producer (a reused registry slot).
  void SetOwner(std::string_view name, ThreadRole role,
                std::uint64_t os_thread_id) noexcept;

  // Producer: marks a record write in progress from |now_ns| until
  // EndWrite(). The log worker writes only records older than every write
  // in progress, so records from different threads come out in timestamp
  // order, and LogWorker::Stop waits for writes in progress.
  void BeginWrite(std::uint64_t now_ns) noexcept {
    write_started_ns_.store(now_ns == 0 ? 1 : now_ns,
                            std::memory_order_seq_cst);
  }
  void EndWrite() noexcept {
    write_started_ns_.store(0, std::memory_order_release);
  }
  // The start of the write in progress, or 0.
  [[nodiscard]] std::uint64_t WriteStartedNs() const noexcept {
    return write_started_ns_.load(std::memory_order_seq_cst);
  }

  // The dump-stream region: the header and the records.
  [[nodiscard]] std::span<const std::byte> StreamBytes() const noexcept;
  [[nodiscard]] const DumpStreamHeader& Header() const noexcept {
    return *header_;
  }
  // Record |index| (head-relative indices are the caller's business).
  [[nodiscard]] const LogRecord& RecordAt(std::uint64_t index) const noexcept {
    return records_[index & mask_];
  }

 private:
  LogRing(void* block, std::uint32_t capacity) noexcept;

  void* block_;
  DumpStreamHeader* header_;
  LogRecord* records_;
  std::uint32_t capacity_;
  std::uint32_t mask_;
  // Producer only: the consumer's tail as last seen.
  std::uint64_t producer_tail_ = 0;
  // Consumer index.
  alignas(64) std::atomic<std::uint64_t> tail_{0};
  // Producer: the write in progress (BeginWrite).
  alignas(64) std::atomic<std::uint64_t> write_started_ns_{0};
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

// The basename of a source path (the part after the last slash).
[[nodiscard]] std::string_view LogBaseName(const char* path) noexcept;

}  // namespace se

#endif  // SE_CORE_LOG_RING_H_
