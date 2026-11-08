// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/log.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string_view>

#include "se_core/breadcrumb.h"
#include "se_core/thread_role.h"

namespace se {
namespace {

std::atomic<LogLevel> g_level{LogLevel::kInfo};
std::atomic<LogSink> g_sink{nullptr};

constexpr std::array<char, 6> kLevelLetters = {'T', 'D', 'I', 'W', 'E', 'F'};

std::string_view BaseName(const char* path) {
  std::string_view view(path);
  const std::size_t slash = view.find_last_of("/\\");
  return slash == std::string_view::npos ? view : view.substr(slash + 1);
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
  const auto role = static_cast<ThreadRole>(crumb.role);
  // Room for the prefix, the message and the location.
  std::array<char, kLogMaxMessage + 160> buffer;
  const auto result = std::format_to_n(
      buffer.data(), buffer.size() - 1, "[{} t={} {}] {} ({}:{})",
      kLevelLetters[static_cast<std::size_t>(level)], crumb.tick,
      RoleName(role), message, BaseName(file), line);
  const auto size = static_cast<std::size_t>(result.size) < buffer.size() - 1
                        ? static_cast<std::size_t>(result.size)
                        : buffer.size() - 1;
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

void LogFatalAbort() noexcept {
  std::fflush(stderr);
  std::abort();
}

}  // namespace se
