// Copyright 2026 The simple-engine-2 Authors.
//
// ThreadRegistry: the process's table of engine threads, 64 slots. Each slot
// records a thread's role, name, OS thread id, breadcrumb and log ring, and
// its state (free, live, exited). The table is one kind-1 dump stream
// (se_core/dump_stream.h): a header and 64 BreadcrumbRecords, whose `packed`
// word is the live Breadcrumb of the slot's thread. The main thread's slot
// is slot 0.
//
// A slot is taken when a thread's RoleScope (se_os) is built, through a
// ThreadSlotScope, and released when it ends; a released slot is reused by
// a later thread. Its log ring is allocated when the slot is first used and
// never freed, and the table itself is never freed, so a crash dump can
// always read them. Registration takes a mutex (thread start and exit
// only); readers (the log worker, the dump code, the watchdog) read
// published slots without it. Threads without a slot (library threads)
// share one mutex-guarded library ring, which the main thread never uses.

#ifndef SE_CORE_THREAD_REGISTRY_H_
#define SE_CORE_THREAD_REGISTRY_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/dump_stream.h"
#include "se_core/log_ring.h"
#include "se_core/thread_role.h"

namespace se {

inline constexpr std::uint32_t kThreadSlotCount = 64;
inline constexpr std::uint32_t kMainThreadSlot = 0;
// Characters of a thread name the registry keeps.
inline constexpr std::size_t kThreadNameSize = 16;
// The index the stream hook reports for the shared library ring.
inline constexpr std::uint32_t kLibraryLogRingIndex = kThreadSlotCount;

// A snapshot of one slot.
struct ThreadSlotInfo {
  ThreadSlotState state = ThreadSlotState::kFree;
  ThreadRole role = ThreadRole::kLibrary;
  std::uint64_t os_thread_id = 0;
  std::uint64_t breadcrumb = 0;
  std::array<char, kThreadNameSize + 1> name{};

  [[nodiscard]] std::string_view Name() const noexcept { return name.data(); }
};

// Called for every dump-stream region of the registry: the breadcrumb table
// (kind 1, index 0), the library ring (kind 2, kLibraryLogRingIndex) and
// each slot's log ring (kind 2, the slot) when it is allocated. Runs under
// the registry mutex: it must not log or start threads.
using DumpStreamHook = void (*)(void* context, DumpStreamKind kind,
                                std::uint32_t index,
                                std::span<const std::byte> region);

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324)  // the table is cache-line aligned on purpose
#endif

class ThreadRegistry {
 public:
  // The process's registry (created on first use, never destroyed).
  [[nodiscard]] static ThreadRegistry& Instance() noexcept;

  ThreadRegistry(const ThreadRegistry&) = delete;
  ThreadRegistry& operator=(const ThreadRegistry&) = delete;

  // Takes a slot for a thread: slot 0 for the main role while it is free,
  // otherwise the first slot (1 and up) that is not live. Allocates the
  // slot's log ring on first use. Returns nullopt, and counts the refusal
  // in the header's `dropped`, when every slot is live.
  [[nodiscard]] std::optional<std::uint32_t> Acquire(
      ThreadRole role, std::string_view name,
      std::uint64_t os_thread_id) noexcept;

  // Marks the slot exited; its record and ring keep their contents.
  void Release(std::uint32_t slot) noexcept;

  // Reads a slot without the mutex.
  [[nodiscard]] ThreadSlotInfo Read(std::uint32_t slot) const noexcept;

  // The slot's breadcrumb word (the record's `packed`).
  [[nodiscard]] std::uint64_t* BreadcrumbWord(std::uint32_t slot) noexcept {
    return &table_.records[slot].packed;
  }

  // The slot's log ring, or nullptr until the slot was first used (or its
  // allocation failed).
  [[nodiscard]] LogRing* SlotLogRing(std::uint32_t slot) const noexcept {
    return rings_[slot].load(std::memory_order_acquire);
  }

  // The ring shared by threads without a slot, and its producers' mutex.
  [[nodiscard]] LogRing& LibraryLogRing() noexcept { return *library_ring_; }
  [[nodiscard]] std::mutex& LibraryLogMutex() noexcept {
    return library_mutex_;
  }

  // Calls f(LogRing&) for every allocated slot ring, in slot order, then
  // for the library ring.
  template <class F>
  void ForEachLogRing(F&& f) const {
    auto&& visit = std::forward<F>(f);
    for (const std::atomic<LogRing*>& ring : rings_) {
      if (LogRing* r = ring.load(std::memory_order_acquire); r != nullptr) {
        visit(*r);
      }
    }
    visit(*library_ring_);
  }

  // The kind-1 dump stream: the header and the 64 records. Live memory:
  // its threads write their records while it is read, which is what a
  // crash handler in another process wants; code in this process copies it
  // with CopyBreadcrumbStream().
  [[nodiscard]] std::span<const std::byte> BreadcrumbStream() const noexcept;
  // A copy of the stream taken field by field with atomic loads (each
  // record as Read() sees it), in the stream's layout: for readers in this
  // process, where copying the live bytes would race with the threads'
  // breadcrumb writes (M10a: the editor's fault files).
  [[nodiscard]] std::vector<std::byte> CopyBreadcrumbStream() const;

  // Slots ever used (the header's head: records 0 .. head - 1 are valid).
  [[nodiscard]] std::uint32_t SlotsUsed() const noexcept;
  // Acquire calls refused because the table was full.
  [[nodiscard]] std::uint64_t Refused() const noexcept;

  // Installs the hook (nullptr removes it) and calls it at once for every
  // region that already exists. The integration registers the regions as
  // crash-dump streams with it.
  void SetStreamHook(DumpStreamHook hook, void* context) noexcept;

 private:
  struct alignas(64) Table {
    DumpStreamHeader header;
    std::array<BreadcrumbRecord, kThreadSlotCount> records;
  };
  static_assert(offsetof(Table, records) == sizeof(DumpStreamHeader));
  static_assert(sizeof(Table) ==
                sizeof(DumpStreamHeader) +
                    kThreadSlotCount * sizeof(BreadcrumbRecord));

  ThreadRegistry() noexcept;
  ~ThreadRegistry() = default;
  static ThreadRegistry* Create() noexcept;

  void CallHook(DumpStreamKind kind, std::uint32_t index,
                std::span<const std::byte> region) const noexcept;

  Table table_;
  std::array<std::atomic<LogRing*>, kThreadSlotCount> rings_{};
  LogRing* library_ring_ = nullptr;
  std::mutex mutex_;
  std::mutex library_mutex_;
  DumpStreamHook hook_ = nullptr;
  void* hook_context_ = nullptr;
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

// Binds the calling thread to a registry slot for the lifetime of the
// scope: the thread's breadcrumb (ThreadBreadcrumb) writes the slot's
// packed word, and its log records go to the slot's ring. RoleScope (se_os)
// makes one, passing the OS thread id; the previous binding (another scope
// on this thread, or none) is restored at the end. When the table is full
// the thread runs without a slot and logs through the library ring.
class ThreadSlotScope {
 public:
  ThreadSlotScope(ThreadRole role, std::string_view name,
                  std::uint64_t os_thread_id) noexcept;
  ~ThreadSlotScope();

  ThreadSlotScope(const ThreadSlotScope&) = delete;
  ThreadSlotScope& operator=(const ThreadSlotScope&) = delete;

  [[nodiscard]] std::optional<std::uint32_t> Slot() const noexcept {
    return slot_;
  }

 private:
  std::optional<std::uint32_t> slot_;
  std::optional<std::uint32_t> previous_slot_;
  LogRing* previous_ring_ = nullptr;
  std::uint64_t* previous_word_ = nullptr;
  std::uint64_t previous_os_id_ = 0;
};

// The calling thread's log ring, or nullptr without a slot.
[[nodiscard]] LogRing* CurrentThreadLogRing() noexcept;
// The calling thread's slot.
[[nodiscard]] std::optional<std::uint32_t> CurrentThreadSlot() noexcept;
// The OS thread id the calling thread registered with, or 0.
[[nodiscard]] std::uint64_t CurrentOsThreadId() noexcept;

}  // namespace se

#endif  // SE_CORE_THREAD_REGISTRY_H_
