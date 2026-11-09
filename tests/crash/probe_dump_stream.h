// Copyright 2026 The simple-engine-2 Authors.
//
// A local copy of the dump stream layout (docs/milestone-specs.md, "Dump
// stream layout"), the contract between M4a and M4b. M4a defines it in
// se_core/dump_stream.h on its branch; the M4 integration replaces this copy
// with that header. The static_asserts pin every size and offset that
// tools/minidump/read_dump.py reads.

#ifndef TESTS_CRASH_PROBE_DUMP_STREAM_H_
#define TESTS_CRASH_PROBE_DUMP_STREAM_H_

#include <cstddef>
#include <cstdint>

#include "se_core/input_frame.h"

namespace se::crash_probe {

inline constexpr std::uint32_t kDumpStreamMagic = 0x31304553;  // "SE01"
inline constexpr std::uint16_t kDumpStreamVersion = 1;

enum class DumpStreamKind : std::uint16_t {
  kBreadcrumbs = 1,
  kLogRing = 2,
  kInputFrameRing = 3,
  kSessionLogTail = 4,
  kFlightRecorder = 5,
};

// The start of every registered region; |capacity| records of
// |record_size| bytes follow it.
struct DumpStreamHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t kind;
  std::uint32_t record_size;
  std::uint32_t capacity;
  // Records written so far; the newest is record (head - 1) % capacity.
  std::uint64_t head;
  std::uint64_t dropped;
  // The producer, 0 when shared.
  std::uint64_t os_thread_id;
  // ThreadRole of the producer.
  std::uint8_t role;
  char name[23];  // NUL-padded
};

enum class SlotState : std::uint8_t {
  kFree = 0,
  kLive = 1,
  kExited = 2,
};

// Kind 1: one per ThreadRegistry slot, one cache line.
struct alignas(64) BreadcrumbRecord {
  std::uint64_t packed;  // the thread's live Breadcrumb word
  std::uint64_t os_thread_id;
  std::uint8_t role;
  std::uint8_t state;  // SlotState
  char name[46];
};

inline constexpr std::size_t kLogFileChars = 36;
inline constexpr std::size_t kLogMessageChars = 448;

// Kind 2.
struct LogRecord {
  std::uint64_t timestamp_ns;  // steady clock
  std::uint64_t sequence;      // per process, for merging
  std::uint32_t tick;
  std::uint32_t line;
  std::uint8_t level;
  std::uint8_t role;
  std::uint16_t length;            // message bytes used
  char file[kLogFileChars];        // basename, NUL-padded
  char message[kLogMessageChars];  // not NUL-terminated
};

// Kind 3.
struct InputFrameSlot {
  std::uint64_t stamp;  // seqlock: odd while being written
  InputFrame frame;     // carries its tick
};

static_assert(sizeof(DumpStreamHeader) == 64);
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

static_assert(sizeof(BreadcrumbRecord) == 64);
static_assert(alignof(BreadcrumbRecord) == 64);
static_assert(offsetof(BreadcrumbRecord, packed) == 0);
static_assert(offsetof(BreadcrumbRecord, os_thread_id) == 8);
static_assert(offsetof(BreadcrumbRecord, role) == 16);
static_assert(offsetof(BreadcrumbRecord, state) == 17);
static_assert(offsetof(BreadcrumbRecord, name) == 18);

static_assert(sizeof(LogRecord) == 512);
static_assert(offsetof(LogRecord, timestamp_ns) == 0);
static_assert(offsetof(LogRecord, sequence) == 8);
static_assert(offsetof(LogRecord, tick) == 16);
static_assert(offsetof(LogRecord, line) == 20);
static_assert(offsetof(LogRecord, level) == 24);
static_assert(offsetof(LogRecord, role) == 25);
static_assert(offsetof(LogRecord, length) == 26);
static_assert(offsetof(LogRecord, file) == 28);
static_assert(offsetof(LogRecord, message) == 64);

// read_dump.py decodes InputFrame at these offsets (InputFrameSlot 968
// bytes); it reports any other record size as an unknown layout.
static_assert(sizeof(InputEdge) == 24);
static_assert(offsetof(InputEdge, action) == 0);
static_assert(offsetof(InputEdge, down) == 1);
static_assert(offsetof(InputEdge, stamp_ns) == 8);
static_assert(offsetof(InputEdge, order) == 16);
static_assert(offsetof(InputEdge, source) == 17);
static_assert(sizeof(InputFrame) == 960);
static_assert(offsetof(InputFrame, tick) == 0);
static_assert(offsetof(InputFrame, held) == 8);
static_assert(offsetof(InputFrame, pressed) == 16);
static_assert(offsetof(InputFrame, released) == 80);
static_assert(offsetof(InputFrame, axes) == 144);
static_assert(offsetof(InputFrame, edge_count) == 176);
static_assert(offsetof(InputFrame, edges) == 184);
static_assert(offsetof(InputFrame, platform_live) == 952);
static_assert(sizeof(InputFrameSlot) == 968);
static_assert(offsetof(InputFrameSlot, frame) == 8);

}  // namespace se::crash_probe

#endif  // TESTS_CRASH_PROBE_DUMP_STREAM_H_
