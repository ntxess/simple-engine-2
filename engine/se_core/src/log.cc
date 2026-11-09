// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string_view>
#include <thread>

#include "se_core/breadcrumb.h"
#include "se_core/dump_stream.h"
#include "se_core/log_ring.h"
#include "se_core/profile_clock.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"

namespace se {
namespace {

static_assert(kLogMaxMessage == kLogRecordMessageSize,
              "a message fills one LogRecord");

// How long SE_LOG_FATAL waits for the worker to write and flush.
constexpr std::uint64_t kFatalFlushTimeoutNs = 2'000'000'000;
// How long DetachLogConsumer waits for ring writes in progress.
constexpr std::uint64_t kDetachTimeoutNs = 1'000'000'000;

std::atomic<LogLevel> g_level{LogLevel::kInfo};
std::atomic<LogSink> g_sink{nullptr};
std::atomic<LogConsumer*> g_consumer{nullptr};
std::atomic<std::uint64_t> g_sequence{0};

constexpr std::array<char, 6> kLevelLetters = {'T', 'D', 'I', 'W', 'E', 'F'};

// Room for the synchronous prefix, the message and the location.
using SyncLine = std::array<char, kLogMaxMessage + 160>;

// The synchronous line of this thread's last fatal record, written to
// stderr by LogFatalAbort when the worker did not flush it.
thread_local SyncLine t_fatal_line;
thread_local std::size_t t_fatal_size = 0;
thread_local bool t_fatal_needs_stderr = false;

// Formats the synchronous line (without the newline); returns its size.
std::size_t FormatSyncLine(LogLevel level, const BreadcrumbFields& crumb,
                           const char* file, int line, std::string_view message,
                           SyncLine& buffer) {
  const auto role = static_cast<ThreadRole>(crumb.role);
  const auto result = std::format_to_n(
      buffer.data(), buffer.size() - 1, "[{} t={} {}] {} ({}:{})",
      kLevelLetters[static_cast<std::size_t>(level)], crumb.tick,
      RoleName(role), message, LogBaseName(file), line);
  return static_cast<std::size_t>(result.size) < buffer.size() - 1
             ? static_cast<std::size_t>(result.size)
             : buffer.size() - 1;
}

void WriteSync(LogLevel level, SyncLine& buffer, std::size_t size) {
  const std::string_view text(buffer.data(), size);
  if (LogSink sink = g_sink.load(std::memory_order_acquire); sink != nullptr) {
    sink(level, text);
    return;
  }
  buffer[size] = '\n';
  std::fwrite(buffer.data(), 1, size + 1, stderr);
  if (level >= LogLevel::kError) {
    std::fflush(stderr);
  }
}

enum class RingResult : std::uint8_t {
  // No consumer: write synchronously.
  kSync,
  // In the ring (and, for a fatal record, flushed by the worker).
  kQueued,
  // Counted as dropped.
  kDropped,
};

void FillRecord(LogRecord& record, LogLevel level, const char* file, int line,
                std::string_view message, const BreadcrumbFields& crumb) {
  record.timestamp_ns = ProfileNowNs();
  record.sequence = g_sequence.fetch_add(1, std::memory_order_relaxed);
  record.tick = crumb.tick;
  record.line = static_cast<std::uint32_t>(line);
  record.level = static_cast<std::uint8_t>(level);
  record.role = crumb.role;
  const std::string_view base = LogBaseName(file);
  const std::size_t file_size = std::min(base.size(), kLogRecordFileSize);
  std::memcpy(record.file, base.data(), file_size);
  const std::size_t size = std::min(message.size(), kLogRecordMessageSize);
  std::memcpy(record.message, message.data(), size);
  record.length = static_cast<std::uint16_t>(size);
}

RingResult WriteToRing(LogLevel level, const char* file, int line,
                       std::string_view message,
                       const BreadcrumbFields& crumb) {
  if (g_consumer.load(std::memory_order_relaxed) == nullptr) {
    return RingResult::kSync;
  }
  LogRing* ring = CurrentThreadLogRing();
  std::unique_lock<std::mutex> lock;
  if (ring == nullptr) {
    ThreadRegistry& registry = ThreadRegistry::Instance();
    ring = &registry.LibraryLogRing();
    lock = std::unique_lock<std::mutex>(registry.LibraryLogMutex(),
                                        std::defer_lock);
    // The main thread has slot 0 while a worker runs; without one it still
    // takes the shared lock only with try_lock.
    if (IsMainThread()) {
      if (!lock.try_lock()) {
        ring->CountDrop();
        if (level == LogLevel::kFatal) t_fatal_needs_stderr = true;
        return RingResult::kDropped;
      }
    } else {
      lock.lock();
    }
  }
  // The mark comes before the consumer check: DetachLogConsumer clears the
  // consumer and then waits for marks, so a record is either pushed while
  // the consumer is still attached or written synchronously.
  ring->BeginWrite(ProfileNowNs());
  LogConsumer* consumer = g_consumer.load(std::memory_order_seq_cst);
  if (consumer == nullptr) {
    ring->EndWrite();
    return RingResult::kSync;
  }
  LogRecord record;
  FillRecord(record, level, file, line, message, crumb);
  const bool pushed = ring->TryPush(record);
  consumer->OnRecordPushed();
  if (level == LogLevel::kFatal) {
    // While the mark is held the consumer cannot be detached.
    t_fatal_needs_stderr = !pushed || !consumer->Flush(kFatalFlushTimeoutNs);
  }
  ring->EndWrite();
  return pushed ? RingResult::kQueued : RingResult::kDropped;
}

}  // namespace

void SetLogLevel(LogLevel level) noexcept {
  g_level.store(level, std::memory_order_relaxed);
}

LogLevel GetLogLevel() noexcept {
  return g_level.load(std::memory_order_relaxed);
}

bool LogEnabled(LogLevel level) noexcept {
  return level >= g_level.load(std::memory_order_relaxed);
}

void SetLogSink(LogSink sink) noexcept {
  g_sink.store(sink, std::memory_order_release);
}

void LogWrite(LogLevel level, const char* file, int line,
              std::string_view message) {
  const BreadcrumbFields crumb = ThreadBreadcrumb().Fields();
  if (level == LogLevel::kFatal) {
    t_fatal_size =
        FormatSyncLine(level, crumb, file, line, message, t_fatal_line);
    t_fatal_needs_stderr = false;
  }
  switch (WriteToRing(level, file, line, message, crumb)) {
    case RingResult::kQueued:
    case RingResult::kDropped:
      return;
    case RingResult::kSync:
      break;
  }
  SyncLine buffer;
  const std::size_t size =
      FormatSyncLine(level, crumb, file, line, message, buffer);
  WriteSync(level, buffer, size);
}

void LogFatalAbort() noexcept {
  if (t_fatal_needs_stderr) {
    t_fatal_line[t_fatal_size] = '\n';
    std::fwrite(t_fatal_line.data(), 1, t_fatal_size + 1, stderr);
  }
  std::fflush(stderr);
  std::abort();
}

std::uint64_t LogEpochNs() noexcept {
  static const std::uint64_t kEpochNs = ProfileNowNs();
  return kEpochNs;
}

bool AttachLogConsumer(LogConsumer* consumer) noexcept {
  LogConsumer* expected = nullptr;
  return consumer != nullptr &&
         g_consumer.compare_exchange_strong(expected, consumer,
                                            std::memory_order_seq_cst);
}

void DetachLogConsumer(LogConsumer* consumer) noexcept {
  LogConsumer* expected = consumer;
  if (!g_consumer.compare_exchange_strong(expected, nullptr,
                                          std::memory_order_seq_cst)) {
    return;
  }
  // Wait for writes that saw the consumer (a few hundred nanoseconds each).
  const std::uint64_t deadline = ProfileNowNs() + kDetachTimeoutNs;
  ThreadRegistry::Instance().ForEachLogRing([&](const LogRing& ring) {
    while (ring.WriteStartedNs() != 0 && ProfileNowNs() < deadline) {
      std::this_thread::yield();
    }
  });
}

bool LogConsumerAttached() noexcept {
  return g_consumer.load(std::memory_order_acquire) != nullptr;
}

}  // namespace se
