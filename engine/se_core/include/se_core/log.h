// Copyright 2026 The simple-engine-2 Authors.
//
// Logging macros:
//
//   SE_LOG_INFO("loaded {} entities in {:.2f} ms", count, ms);
//
// The format string is checked at compile time (std::format_string). Levels
// below SE_LOG_MIN_LEVEL are stripped at compile time: the call is still
// type-checked but generates no code. Each line carries the thread role and
// the sim tick from the thread's breadcrumb.
//
// Backend (M4). While a log consumer (se_log's LogWorker) is attached, each
// record goes into the calling thread's LogRing (se_core/log_ring.h; the
// thread's ThreadRegistry slot, or the shared library ring for threads
// without one) and the worker writes it; a full ring drops the record and
// counts it, so logging never waits. Without a consumer (tests, tools,
// before boot finishes, after shutdown) records are written synchronously
// to stderr, or to the sink a test installs. SE_LOG_FATAL logs, has the
// worker flush its sinks (or writes to stderr itself), and aborts; it is
// never stripped.

#ifndef SE_CORE_LOG_H_
#define SE_CORE_LOG_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

namespace se {

enum class LogLevel : std::uint8_t {
  kTrace,
  kDebug,
  kInfo,
  kWarn,
  kError,
  kFatal,
};

// Compile-time minimum level: 0 trace ... 5 fatal. Debug and Dev builds keep
// trace and debug; Release keeps info and above.
#ifndef SE_LOG_MIN_LEVEL
#if SE_ASSERTS
#define SE_LOG_MIN_LEVEL 0
#else
#define SE_LOG_MIN_LEVEL 2
#endif
#endif
inline constexpr int kLogMinLevel = SE_LOG_MIN_LEVEL;

// The longest message kept (LogRecord::message); longer messages are
// truncated.
inline constexpr std::size_t kLogMaxMessage = 448;

// Sets the run-time threshold (default kInfo).
void SetLogLevel(LogLevel level) noexcept;
[[nodiscard]] LogLevel GetLogLevel() noexcept;
[[nodiscard]] bool LogEnabled(LogLevel level) noexcept;

// Receives each formatted line (without the newline). Tests install one to
// capture output; nullptr restores stderr.
using LogSink = void (*)(LogLevel level, std::string_view line);
void SetLogSink(LogSink sink) noexcept;

// Writes one already formatted message.
void LogWrite(LogLevel level, const char* file, int line,
              std::string_view message);

// Flushes, then aborts the process (SE_LOG_FATAL).
[[noreturn]] void LogFatalAbort() noexcept;

// Writes one formatted line (without its newline) where synchronous lines
// go: the sink a test installed, or stderr. The log worker's console sink
// writes through it, so a test's sink receives the lines with or without
// a worker.
void WriteConsoleLine(LogLevel level, std::string_view line);

// Has the attached consumer write and flush every record pushed so far,
// waiting at most |timeout_ns| (the watchdog's shutdown deadline, before it
// ends the process); without one, flushes stderr. Returns false when the
// flush did not finish in time, or cannot be done from the calling thread
// (a thread without a ring of its own, or the consumer's own thread).
bool FlushLog(std::uint64_t timeout_ns) noexcept;

// The origin of the log's time column: ProfileNowNs() when it was first
// read.
[[nodiscard]] std::uint64_t LogEpochNs() noexcept;

// The receiver of ring records: the log worker (se_log). The front end calls
// OnRecordPushed after each push into a ring, from the pushing thread, and
// Flush from SE_LOG_FATAL.
class LogConsumer {
 public:
  // A record was pushed (any thread; must not block).
  virtual void OnRecordPushed() noexcept = 0;
  // Writes every record pushed so far and flushes the sinks; returns false
  // when that did not finish within |timeout_ns| or cannot be done from the
  // calling thread.
  virtual bool Flush(std::uint64_t timeout_ns) noexcept = 0;

  virtual ~LogConsumer() = default;

 protected:
  LogConsumer() = default;
  LogConsumer(const LogConsumer&) = default;
  LogConsumer& operator=(const LogConsumer&) = default;
};

// Routes records to |consumer|'s rings from now on. Returns false when
// another consumer is attached.
[[nodiscard]] bool AttachLogConsumer(LogConsumer* consumer) noexcept;

// Returns the front end to the synchronous path. When it returns, no thread
// is writing a ring record any more (it waits for writes in progress), so
// the consumer can drain the rings a last time.
void DetachLogConsumer(LogConsumer* consumer) noexcept;

// True while a consumer is attached.
[[nodiscard]] bool LogConsumerAttached() noexcept;

// Formats into a fixed buffer (no allocation) and writes the message.
// Formatting can throw std::format_error for invalid dynamic arguments (a
// negative width, say), so logging functions are not noexcept and noexcept
// functions do not log.
template <class... Args>
void LogFormat(LogLevel level, const char* file, int line,
               std::format_string<Args...> fmt, Args&&... args) {
  std::array<char, kLogMaxMessage> buffer;
  const auto result = std::format_to_n(buffer.data(), buffer.size(), fmt,
                                       std::forward<Args>(args)...);
  const auto size = static_cast<std::size_t>(result.size) < buffer.size()
                        ? static_cast<std::size_t>(result.size)
                        : buffer.size();
  LogWrite(level, file, line, std::string_view(buffer.data(), size));
}

}  // namespace se

#define SE_LOG_AT_LEVEL(level, ...)                                \
  do {                                                             \
    if constexpr (static_cast<int>(level) >= ::se::kLogMinLevel) { \
      if (::se::LogEnabled(level)) {                               \
        ::se::LogFormat((level), __FILE__, __LINE__, __VA_ARGS__); \
      }                                                            \
    }                                                              \
  } while (false)

#define SE_LOG_TRACE(...) SE_LOG_AT_LEVEL(::se::LogLevel::kTrace, __VA_ARGS__)
#define SE_LOG_DEBUG(...) SE_LOG_AT_LEVEL(::se::LogLevel::kDebug, __VA_ARGS__)
#define SE_LOG_INFO(...) SE_LOG_AT_LEVEL(::se::LogLevel::kInfo, __VA_ARGS__)
#define SE_LOG_WARN(...) SE_LOG_AT_LEVEL(::se::LogLevel::kWarn, __VA_ARGS__)
#define SE_LOG_ERROR(...) SE_LOG_AT_LEVEL(::se::LogLevel::kError, __VA_ARGS__)
#define SE_LOG_FATAL(...)                                                     \
  do {                                                                        \
    ::se::LogFormat(::se::LogLevel::kFatal, __FILE__, __LINE__, __VA_ARGS__); \
    ::se::LogFatalAbort();                                                    \
  } while (false)

#endif  // SE_CORE_LOG_H_
