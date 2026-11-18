// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/thread_registry.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "se_core/assert.h"
#include "se_core/breadcrumb.h"
#include "se_core/dump_stream.h"
#include "se_core/log_ring.h"
#include "se_core/thread_role.h"

namespace se {
namespace {

// The calling thread's binding (ThreadSlotScope).
thread_local LogRing* t_log_ring = nullptr;
thread_local std::uint64_t t_os_thread_id = 0;
thread_local std::int32_t t_slot = -1;

std::span<const std::byte> AsBytes(const void* data, std::size_t size) {
  return {static_cast<const std::byte*>(data), size};
}

}  // namespace

ThreadRegistry& ThreadRegistry::Instance() noexcept {
  static ThreadRegistry* const kRegistry = Create();
  return *kRegistry;
}

ThreadRegistry* ThreadRegistry::Create() noexcept {
  auto* registry = new (std::nothrow) ThreadRegistry();
  if (registry == nullptr) {
    std::fputs("out of memory for the thread registry\n", stderr);
    std::abort();
  }
  return registry;
}

ThreadRegistry::ThreadRegistry() noexcept {
  InitDumpStreamHeader(table_.header, DumpStreamKind::kBreadcrumbs,
                       sizeof(BreadcrumbRecord), kThreadSlotCount,
                       "breadcrumbs");
  // Never freed, like the slot rings: a dump may read it at any time.
  library_ring_ = LogRing::Create(kLibraryLogRingCapacity, "library",
                                  ThreadRole::kLibrary, 0)
                      .release();
  SE_ASSERT_MSG(library_ring_ != nullptr, "out of memory for the log ring");
}

std::optional<std::uint32_t> ThreadRegistry::Acquire(
    ThreadRole role, std::string_view name,
    std::uint64_t os_thread_id) noexcept {
  const std::scoped_lock lock(mutex_);
  const auto live = [this](std::uint32_t slot) {
    return AtomicLoad(table_.records[slot].state, std::memory_order_relaxed) ==
           static_cast<std::uint8_t>(ThreadSlotState::kLive);
  };
  std::optional<std::uint32_t> chosen;
  if (role == ThreadRole::kMain && !live(kMainThreadSlot)) {
    chosen = kMainThreadSlot;
  } else {
    for (std::uint32_t slot = 1; slot < kThreadSlotCount; ++slot) {
      if (!live(slot)) {
        chosen = slot;
        break;
      }
    }
  }
  if (!chosen) {
    std::atomic_ref<std::uint64_t>(table_.header.dropped)
        .fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
  }
  const std::uint32_t slot = *chosen;
  BreadcrumbRecord& record = table_.records[slot];
  AtomicStore(record.os_thread_id, os_thread_id, std::memory_order_relaxed);
  AtomicStore(record.role, static_cast<std::uint8_t>(role),
              std::memory_order_relaxed);
  StoreDumpName(std::span<char>(record.name).first(kThreadNameSize),
                name.substr(0, kThreadNameSize));
  StoreDumpName(std::span<char>(record.name).subspan(kThreadNameSize), {});

  if (LogRing* ring = rings_[slot].load(std::memory_order_relaxed);
      ring != nullptr) {
    ring->SetOwner(name.substr(0, kThreadNameSize), role, os_thread_id);
  } else {
    const std::uint32_t capacity =
        slot == kMainThreadSlot ? kMainLogRingCapacity : kThreadLogRingCapacity;
    ring = LogRing::Create(capacity, name.substr(0, kThreadNameSize), role,
                           os_thread_id)
               .release();
    if (ring != nullptr) {
      rings_[slot].store(ring, std::memory_order_release);
      CallHook(DumpStreamKind::kLogRing, slot, ring->StreamBytes());
    }
  }

  const std::atomic_ref<std::uint64_t> used(table_.header.head);
  used.store(std::max<std::uint64_t>(used.load(std::memory_order_relaxed),
                                     slot + 1ULL),
             std::memory_order_relaxed);
  AtomicStore(record.state, static_cast<std::uint8_t>(ThreadSlotState::kLive),
              std::memory_order_release);
  return slot;
}

void ThreadRegistry::Release(std::uint32_t slot) noexcept {
  SE_ASSERT_MSG(slot < kThreadSlotCount, "no such ThreadRegistry slot");
  const std::scoped_lock lock(mutex_);
  AtomicStore(table_.records[slot].state,
              static_cast<std::uint8_t>(ThreadSlotState::kExited),
              std::memory_order_release);
}

ThreadSlotInfo ThreadRegistry::Read(std::uint32_t slot) const noexcept {
  SE_ASSERT_MSG(slot < kThreadSlotCount, "no such ThreadRegistry slot");
  const BreadcrumbRecord& record = table_.records[slot];
  ThreadSlotInfo info;
  info.state = static_cast<ThreadSlotState>(
      AtomicLoad(record.state, std::memory_order_acquire));
  info.role = static_cast<ThreadRole>(
      AtomicLoad(record.role, std::memory_order_relaxed));
  info.os_thread_id =
      AtomicLoad(record.os_thread_id, std::memory_order_relaxed);
  info.breadcrumb = AtomicLoad(record.packed, std::memory_order_relaxed);
  const auto name = LoadDumpName(record.name);
  std::copy_n(name.begin(), kThreadNameSize, info.name.begin());
  return info;
}

std::span<const std::byte> ThreadRegistry::BreadcrumbStream() const noexcept {
  return AsBytes(&table_, sizeof(table_));
}

std::vector<std::byte> ThreadRegistry::CopyBreadcrumbStream() const {
  std::vector<std::byte> bytes(sizeof(Table));
  // The header's other fields never change after construction.
  DumpStreamHeader header;
  InitDumpStreamHeader(header, DumpStreamKind::kBreadcrumbs,
                       sizeof(BreadcrumbRecord), kThreadSlotCount,
                       "breadcrumbs");
  header.head = AtomicLoad(table_.header.head, std::memory_order_relaxed);
  header.dropped = AtomicLoad(table_.header.dropped, std::memory_order_relaxed);
  std::memcpy(bytes.data(), &header, sizeof(header));
  for (std::uint32_t slot = 0; slot < kThreadSlotCount; ++slot) {
    const BreadcrumbRecord& live = table_.records[slot];
    // Field by field into the zeroed bytes: no padding is copied.
    std::byte* out = bytes.data() + offsetof(Table, records) +
                     slot * sizeof(BreadcrumbRecord);
    const std::uint8_t state =
        AtomicLoad(live.state, std::memory_order_acquire);
    const std::uint8_t role = AtomicLoad(live.role, std::memory_order_relaxed);
    const std::uint64_t os_thread_id =
        AtomicLoad(live.os_thread_id, std::memory_order_relaxed);
    const std::uint64_t packed =
        AtomicLoad(live.packed, std::memory_order_relaxed);
    const auto name = LoadDumpName(live.name);
    std::memcpy(out + offsetof(BreadcrumbRecord, packed), &packed,
                sizeof(packed));
    std::memcpy(out + offsetof(BreadcrumbRecord, os_thread_id), &os_thread_id,
                sizeof(os_thread_id));
    std::memcpy(out + offsetof(BreadcrumbRecord, role), &role, sizeof(role));
    std::memcpy(out + offsetof(BreadcrumbRecord, state), &state, sizeof(state));
    std::memcpy(out + offsetof(BreadcrumbRecord, name), name.data(),
                sizeof(live.name));
  }
  return bytes;
}

std::uint32_t ThreadRegistry::SlotsUsed() const noexcept {
  return static_cast<std::uint32_t>(
      AtomicLoad(table_.header.head, std::memory_order_relaxed));
}

std::uint64_t ThreadRegistry::Refused() const noexcept {
  return AtomicLoad(table_.header.dropped, std::memory_order_relaxed);
}

void ThreadRegistry::SetStreamHook(DumpStreamHook hook,
                                   void* context) noexcept {
  const std::scoped_lock lock(mutex_);
  hook_ = hook;
  hook_context_ = context;
  CallHook(DumpStreamKind::kBreadcrumbs, 0, BreadcrumbStream());
  CallHook(DumpStreamKind::kLogRing, kLibraryLogRingIndex,
           library_ring_->StreamBytes());
  for (std::uint32_t slot = 0; slot < kThreadSlotCount; ++slot) {
    if (const LogRing* ring = rings_[slot].load(std::memory_order_relaxed);
        ring != nullptr) {
      CallHook(DumpStreamKind::kLogRing, slot, ring->StreamBytes());
    }
  }
}

void ThreadRegistry::CallHook(
    DumpStreamKind kind, std::uint32_t index,
    std::span<const std::byte> region) const noexcept {
  if (hook_ != nullptr) hook_(hook_context_, kind, index, region);
}

ThreadSlotScope::ThreadSlotScope(ThreadRole role, std::string_view name,
                                 std::uint64_t os_thread_id) noexcept
    : slot_(ThreadRegistry::Instance().Acquire(role, name, os_thread_id)),
      previous_ring_(t_log_ring),
      previous_word_(ThreadBreadcrumb().BoundWord()),
      previous_os_id_(t_os_thread_id) {
  if (t_slot >= 0) previous_slot_ = static_cast<std::uint32_t>(t_slot);
  t_os_thread_id = os_thread_id;
  if (!slot_) {
    // No slot: the thread logs through the library ring and keeps its own
    // breadcrumb word.
    t_log_ring = nullptr;
    t_slot = -1;
    ThreadBreadcrumb().Bind(nullptr);
    return;
  }
  ThreadRegistry& registry = ThreadRegistry::Instance();
  t_log_ring = registry.SlotLogRing(*slot_);
  t_slot = static_cast<std::int32_t>(*slot_);
  Breadcrumb& crumb = ThreadBreadcrumb();
  crumb.Bind(registry.BreadcrumbWord(*slot_));
  crumb.SetRole(role);
}

ThreadSlotScope::~ThreadSlotScope() {
  ThreadBreadcrumb().Bind(previous_word_);
  t_log_ring = previous_ring_;
  t_os_thread_id = previous_os_id_;
  t_slot = previous_slot_ ? static_cast<std::int32_t>(*previous_slot_) : -1;
  if (slot_) ThreadRegistry::Instance().Release(*slot_);
}

LogRing* CurrentThreadLogRing() noexcept { return t_log_ring; }

std::optional<std::uint32_t> CurrentThreadSlot() noexcept {
  if (t_slot < 0) return std::nullopt;
  return static_cast<std::uint32_t>(t_slot);
}

std::uint64_t CurrentOsThreadId() noexcept { return t_os_thread_id; }

}  // namespace se
