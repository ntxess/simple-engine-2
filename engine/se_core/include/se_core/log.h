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
// M2 writes synchronously to stderr. M4 replaces the backend with per-thread
// rings and a log worker without touching call sites. SE_LOG_FATAL logs,
// flushes and aborts; it is never stripped.

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

// The longest message kept; longer messages are truncated.
inline constexpr std::size_t kLogMaxMessage = 480;

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
