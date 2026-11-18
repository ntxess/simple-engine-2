// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/log_ring.h"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <string_view>

#include "se_core/assert.h"
#include "se_core/dump_stream.h"
#include "se_core/thread_role.h"

namespace se {
namespace {

constexpr std::align_val_t kBlockAlignment{64};

std::size_t BlockSize(std::uint32_t capacity) {
  return sizeof(DumpStreamHeader) +
         static_cast<std::size_t>(capacity) * sizeof(LogRecord);
}

}  // namespace

std::unique_ptr<LogRing> LogRing::Create(std::uint32_t capacity,
                                         std::string_view name, ThreadRole role,
                                         std::uint64_t os_thread_id) noexcept {
  SE_ASSERT_MSG(std::has_single_bit(capacity),
                "LogRing capacity must be a power of two");
  capacity = std::bit_ceil(capacity < 1 ? 1U : capacity);
  void* block =
      ::operator new(BlockSize(capacity), kBlockAlignment, std::nothrow);
  if (block == nullptr) return nullptr;
  // The ring object is small; the block is the dump stream.
  std::unique_ptr<LogRing> ring(new (std::nothrow) LogRing(block, capacity));
  if (ring == nullptr) {
    ::operator delete(block, kBlockAlignment);
    return nullptr;
  }
  ring->SetOwner(name, role, os_thread_id);
  return ring;
}

LogRing::LogRing(void* block, std::uint32_t capacity) noexcept
    : block_(block),
      header_(new (block) DumpStreamHeader{}),
      records_(reinterpret_cast<LogRecord*>(static_cast<std::byte*>(block) +
                                            sizeof(DumpStreamHeader))),
      capacity_(capacity),
      mask_(capacity - 1) {
  InitDumpStreamHeader(*header_, DumpStreamKind::kLogRing, sizeof(LogRecord),
                       capacity, {});
  std::uninitialized_value_construct_n(records_, capacity);
}

LogRing::~LogRing() { ::operator delete(block_, kBlockAlignment); }

bool LogRing::TryPush(const LogRecord& record) noexcept {
  const std::atomic_ref<std::uint64_t> head_ref(header_->head);
  const std::uint64_t head = head_ref.load(std::memory_order_relaxed);
  if (head - producer_tail_ >= capacity_) {
    producer_tail_ = tail_.load(std::memory_order_acquire);
    if (head - producer_tail_ >= capacity_) {
      CountDrop();
      return false;
    }
  }
  records_[head & mask_] = record;
  head_ref.store(head + 1, std::memory_order_release);
  return true;
}

void LogRing::CountDrop() noexcept {
  std::atomic_ref<std::uint64_t>(header_->dropped)
      .fetch_add(1, std::memory_order_relaxed);
}

void LogRing::SetOwner(std::string_view name, ThreadRole role,
                       std::uint64_t os_thread_id) noexcept {
  AtomicStore(header_->os_thread_id, os_thread_id, std::memory_order_relaxed);
  AtomicStore(header_->role, static_cast<std::uint8_t>(role),
              std::memory_order_relaxed);
  StoreDumpName(header_->name, name);
}

std::span<const std::byte> LogRing::StreamBytes() const noexcept {
  return {static_cast<const std::byte*>(block_), BlockSize(capacity_)};
}

std::string_view LogBaseName(const char* path) noexcept {
  const std::string_view view(path);
  const std::size_t slash = view.find_last_of("/\\");
  return slash == std::string_view::npos ? view : view.substr(slash + 1);
}

}  // namespace se
