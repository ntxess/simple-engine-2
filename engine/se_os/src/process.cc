// Copyright 2026 The simple-engine-2 Authors.

#include "se_os/process.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "se_core/error.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace se {

std::expected<std::string, Error> ExecutableDir() {
#if defined(_WIN32)
  // Long paths: grow the buffer until the name fits.
  std::vector<wchar_t> buffer(MAX_PATH);
  DWORD size = 0;
  for (;;) {
    size = GetModuleFileNameW(nullptr, buffer.data(),
                              static_cast<DWORD>(buffer.size()));
    if (size == 0) {
      return Fail(ErrorCode::kIo, std::format("GetModuleFileNameW failed "
                                              "(error {})",
                                              GetLastError()));
    }
    if (size < buffer.size()) break;
    buffer.resize(buffer.size() * 2);
  }
  std::wstring path(buffer.data(), size);
  const std::size_t slash = path.find_last_of(L"\\/");
  if (slash != std::wstring::npos) path.resize(slash);
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.data(),
                                        static_cast<int>(path.size()), nullptr,
                                        0, nullptr, nullptr);
  std::string utf8(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
  if (bytes > 0) {
    WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                        utf8.data(), bytes, nullptr, nullptr);
  }
  return utf8;
#else
  std::vector<char> buffer(4096);
  const ssize_t size = readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (size <= 0 || static_cast<std::size_t>(size) >= buffer.size()) {
    return Fail(ErrorCode::kIo, "cannot read /proc/self/exe");
  }
  std::string path(buffer.data(), static_cast<std::size_t>(size));
  const std::size_t slash = path.find_last_of('/');
  if (slash != std::string::npos) path.resize(slash == 0 ? 1 : slash);
  return path;
#endif
}

std::optional<SystemCpuTimes> ReadSystemCpuTimes() {
#if defined(_WIN32)
  FILETIME idle{};
  FILETIME kernel{};
  FILETIME user{};
  if (GetSystemTimes(&idle, &kernel, &user) == FALSE) return std::nullopt;
  const auto ticks = [](const FILETIME& t) {
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) |
           t.dwLowDateTime;
  };
  // 100 ns units; kernel time includes the idle time.
  SystemCpuTimes times;
  times.idle_ns = ticks(idle) * 100;
  times.total_ns = (ticks(kernel) + ticks(user)) * 100;
  return times;
#else
  std::FILE* file = std::fopen("/proc/stat", "rb");
  if (file == nullptr) return std::nullopt;
  char line[256] = {};
  const bool got = std::fgets(line, sizeof(line), file) != nullptr;
  std::fclose(file);
  if (!got) return std::nullopt;
  // "cpu  user nice system idle iowait irq softirq ..." in clock ticks.
  std::string_view text(line);
  if (!text.starts_with("cpu ")) return std::nullopt;
  text.remove_prefix(4);
  std::uint64_t fields[7] = {};
  for (std::uint64_t& field : fields) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), field);
    if (error != std::errc()) return std::nullopt;
    text.remove_prefix(static_cast<std::size_t>(end - text.data()));
  }
  const auto hz = sysconf(_SC_CLK_TCK);
  if (hz <= 0) return std::nullopt;
  const std::uint64_t ns_per_tick =
      1'000'000'000ULL / static_cast<std::uint64_t>(hz);
  SystemCpuTimes times;
  times.idle_ns = (fields[3] + fields[4]) * ns_per_tick;
  std::uint64_t total = 0;
  for (const std::uint64_t field : fields) total += field;
  times.total_ns = total * ns_per_tick;
  return times;
#endif
}

}  // namespace se
