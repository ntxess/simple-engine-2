// Copyright 2026 The simple-engine-2 Authors.

#include "se_os/debugger.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string_view>
#endif

namespace se {

bool DebuggerAttached() noexcept {
#if defined(_WIN32)
  return IsDebuggerPresent() != FALSE;
#else
  std::FILE* status = std::fopen("/proc/self/status", "r");
  if (status == nullptr) return false;
  char line[256];
  bool attached = false;
  constexpr std::string_view kTracer = "TracerPid:";
  while (std::fgets(line, sizeof(line), status) != nullptr) {
    const std::string_view text(line);
    if (!text.starts_with(kTracer)) continue;
    std::string_view rest = text.substr(kTracer.size());
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
      rest.remove_prefix(1);
    }
    int pid = 0;
    std::from_chars(rest.data(), rest.data() + rest.size(), pid);
    attached = pid != 0;
    break;
  }
  std::fclose(status);
  return attached;
#endif
}

}  // namespace se
