// Copyright 2026 The simple-engine-2 Authors.
//
// The layout of every memory region registered as a crash-dump stream
// (docs/milestone-specs.md, M4 "Dump stream layout"; the contract between
// the rings of M4a and the crash capture and tools/minidump/read_dump.py of
// M4b). A region is one contiguous allocation that is never freed: a
// DumpStreamHeader followed by `capacity` records of `record_size` bytes.
// Little-endian, natural alignment. The producer's live counters are the
// header fields themselves, so a dump shows the real state.
//
//   DumpStreamHeader  64 bytes
//     0  u32 magic         0x31304553, "SE01"
//     4  u16 version       1
//     6  u16 kind          DumpStreamKind
//     8  u32 record_size
//    12  u32 capacity
//    16  u64 head          records written so far; the newest is record
//                          (head - 1) % capacity
//    24  u64 dropped
//    32  u64 os_thread_id  the producer, 0 when shared
//    40  u8  role          ThreadRole of the producer
//    41  char name[23]     NUL-padded
//
//   BreadcrumbRecord  kind 1, one per ThreadRegistry slot, 64 bytes
//     0  u64 packed        the thread's live Breadcrumb word
//     8  u64 os_thread_id
//    16  u8  role
//    17  u8  state         0 free, 1 live, 2 exited
//    18  char name[46]
//
//   LogRecord  kind 2, 512 bytes
//     0  u64 timestamp_ns  steady clock (ProfileNowNs)
//     8  u64 sequence      per process, for merging
//    16  u32 tick
//    20  u32 line
//    24  u8  level         LogLevel
//    25  u8  role
//    26  u16 length        message bytes used
//    28  char file[36]     basename, NUL-padded
//    64  char message[448] not NUL-terminated
//
//   InputFrameSlot  kind 3, 968 bytes
//     0  u64 stamp         seqlock: 2n + 1 while frame n is written, 2n + 2
//                          once it is complete
//     8  InputFrame frame  (its layout is pinned below)
//
// The structs are plain data so the layout can be pinned with static_assert.
// Fields that change while other threads (or the dump writer) read them are
// accessed through std::atomic_ref (AtomicLoad, AtomicStore, the name
// helpers below).

#ifndef SE_CORE_DUMP_STREAM_H_
#define SE_CORE_DUMP_STREAM_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include "se_core/input_frame.h"

namespace se {

inline constexpr std::uint32_t kDumpStreamMagic = 0x31304553;  // "SE01"
inline constexpr std::uint16_t kDumpStreamVersion = 1;
inline constexpr std::size_t kDumpStreamNameSize = 23;
inline constexpr std::size_t kBreadcrumbRecordNameSize = 46;
inline constexpr std::size_t kLogRecordFileSize = 36;
inline constexpr std::size_t kLogRecordMessageSize = 448;

enum class DumpStreamKind : std::uint16_t {
  kBreadcrumbs = 1,
  kLogRing = 2,
  kInputFrameRing = 3,
  kSessionLogTail = 4,
  kFlightRecorder = 5,
};

struct DumpStreamHeader {
  std::uint32_t magic = kDumpStreamMagic;
  std::uint16_t version = kDumpStreamVersion;
  std::uint16_t kind = 0;
  std::uint32_t record_size = 0;
  std::uint32_t capacity = 0;
  std::uint64_t head = 0;
  std::uint64_t dropped = 0;
  std::uint64_t os_thread_id = 0;
  std::uint8_t role = 0;
  char name[kDumpStreamNameSize] = {};
};

// The state byte of a BreadcrumbRecord.
enum class ThreadSlotState : std::uint8_t {
  kFree = 0,
  kLive = 1,
  kExited = 2,
};

struct alignas(64) BreadcrumbRecord {
  std::uint64_t packed = 0;
  std::uint64_t os_thread_id = 0;
  std::uint8_t role = 0;
  std::uint8_t state = 0;
  char name[kBreadcrumbRecordNameSize] = {};
};

struct LogRecord {
  std::uint64_t timestamp_ns = 0;
  std::uint64_t sequence = 0;
  std::uint32_t tick = 0;
  std::uint32_t line = 0;
  std::uint8_t level = 0;
  std::uint8_t role = 0;
  std::uint16_t length = 0;
  char file[kLogRecordFileSize] = {};
  char message[kLogRecordMessageSize] = {};

  [[nodiscard]] std::string_view Message() const noexcept {
    return {message, length};
  }
  // The basename up to its first NUL.
  [[nodiscard]] std::string_view File() const noexcept;
};

struct InputFrameSlot {
  std::uint64_t stamp = 0;
  InputFrame frame;
};

// Pinned layouts: tools/minidump/read_dump.py decodes them without the build.
static_assert(std::is_standard_layout_v<DumpStreamHeader>);
static_assert(std::is_trivially_copyable_v<DumpStreamHeader>);
static_assert(sizeof(DumpStreamHeader) == 64);
static_assert(alignof(DumpStreamHeader) == 8);
static_assert(offsetof(DumpStreamHeader, magic) == 0);
static_assert(offsetof(DumpStreamHeader, version) == 4);
static_assert(offsetof(DumpStreamHeader, kind) == 6);
static_assert(offsetof(DumpStreamHeader, record_size) == 8);
static_assert(offsetof(DumpStreamHeader, capacity) == 12);
static_assert(offsetof(DumpStreamHeader, head) == 16);
static_assert(offsetof(DumpStreamHeader, dropped) == 24);
static_assert(offsetof(DumpStreamHeader, os_thread_id) == 32);
static_assert(offsetof(DumpStreamHeader, role) == 40);
static_assert(offsetof(DumpStreamHeader, name) == 41);

static_assert(std::is_standard_layout_v<BreadcrumbRecord>);
static_assert(std::is_trivially_copyable_v<BreadcrumbRecord>);
static_assert(sizeof(BreadcrumbRecord) == 64);
static_assert(alignof(BreadcrumbRecord) == 64);
static_assert(offsetof(BreadcrumbRecord, packed) == 0);
static_assert(offsetof(BreadcrumbRecord, os_thread_id) == 8);
static_assert(offsetof(BreadcrumbRecord, role) == 16);
static_assert(offsetof(BreadcrumbRecord, state) == 17);
static_assert(offsetof(BreadcrumbRecord, name) == 18);

static_assert(std::is_standard_layout_v<LogRecord>);
static_assert(std::is_trivially_copyable_v<LogRecord>);
static_assert(sizeof(LogRecord) == 512);
static_assert(alignof(LogRecord) == 8);
static_assert(offsetof(LogRecord, timestamp_ns) == 0);
static_assert(offsetof(LogRecord, sequence) == 8);
static_assert(offsetof(LogRecord, tick) == 16);
static_assert(offsetof(LogRecord, line) == 20);
static_assert(offsetof(LogRecord, level) == 24);
static_assert(offsetof(LogRecord, role) == 25);
static_assert(offsetof(LogRecord, length) == 26);
static_assert(offsetof(LogRecord, file) == 28);
static_assert(offsetof(LogRecord, message) == 64);

// InputEdge and InputFrame (se_core/input_frame.h) as the ring stores them.
static_assert(std::is_standard_layout_v<InputEdge>);
static_assert(sizeof(InputEdge) == 24);
static_assert(offsetof(InputEdge, action) == 0);
static_assert(offsetof(InputEdge, down) == 1);
static_assert(offsetof(InputEdge, stamp_ns) == 8);
static_assert(offsetof(InputEdge, order) == 16);
static_assert(offsetof(InputEdge, source) == 17);
static_assert(std::is_standard_layout_v<InputFrame>);
static_assert(sizeof(InputFrame) == 960);
static_assert(alignof(InputFrame) == 8);
static_assert(offsetof(InputFrame, tick) == 0);
static_assert(offsetof(InputFrame, held) == 8);
static_assert(offsetof(InputFrame, pressed) == 16);
static_assert(offsetof(InputFrame, released) == 80);
static_assert(offsetof(InputFrame, axes) == 144);
static_assert(offsetof(InputFrame, edge_count) == 176);
static_assert(offsetof(InputFrame, edges) == 184);
static_assert(offsetof(InputFrame, platform_live) == 952);

static_assert(std::is_standard_layout_v<InputFrameSlot>);
static_assert(sizeof(InputFrameSlot) == 968);
static_assert(offsetof(InputFrameSlot, stamp) == 0);
static_assert(offsetof(InputFrameSlot, frame) == 8);

static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint8_t>::is_always_lock_free);
static_assert(std::atomic_ref<char>::is_always_lock_free);

// Atomic access to a field of a dump-stream struct. The objects are never
// const themselves, so a const reference may be read through atomic_ref.
template <class T>
[[nodiscard]] T AtomicLoad(const T& field, std::memory_order order) noexcept {
  return std::atomic_ref<T>(const_cast<T&>(field)).load(order);
}

template <class T>
void AtomicStore(T& field, T value, std::memory_order order) noexcept {
  std::atomic_ref<T>(field).store(value, order);
}

// Writes |name| into a NUL-padded name field, one relaxed atomic byte store
// at a time, truncated to the field.
void StoreDumpName(std::span<char> field, std::string_view name) noexcept;

// Reads a name field written by StoreDumpName (relaxed atomic byte loads),
// up to its first NUL. The result is NUL-terminated.
template <std::size_t N>
[[nodiscard]] std::array<char, N + 1> LoadDumpName(
    const char (&field)[N]) noexcept {
  std::array<char, N + 1> out{};
  for (std::size_t i = 0; i < N; ++i) {
    const char c = AtomicLoad(field[i], std::memory_order_relaxed);
    if (c == '\0') break;
    out[i] = c;
  }
  return out;
}

// Fills a header for a region of |capacity| records of kind |kind|.
void InitDumpStreamHeader(DumpStreamHeader& header, DumpStreamKind kind,
                          std::uint32_t record_size, std::uint32_t capacity,
                          std::string_view name) noexcept;

}  // namespace se

#endif  // SE_CORE_DUMP_STREAM_H_
