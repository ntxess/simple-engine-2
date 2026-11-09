// Copyright 2026 The simple-engine-2 Authors.

#include "se_os/process.h"

#include <cstddef>
#include <expected>
#include <format>
#include <string>
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

}  // namespace se
