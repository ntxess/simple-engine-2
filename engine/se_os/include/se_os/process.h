// Copyright 2026 The simple-engine-2 Authors.
//
// Process facts needed before SDL is initialised: the crash handler starts
// first and looks for its handler executable next to the program.

#ifndef SE_OS_PROCESS_H_
#define SE_OS_PROCESS_H_

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "se_core/error.h"

namespace se {

// The directory of the running executable, UTF-8, without a trailing
// separator (GetModuleFileNameW; /proc/self/exe).
[[nodiscard]] std::expected<std::string, Error> ExecutableDir();

// The machine's CPU time so far, summed over its logical processors
// (GetSystemTimes; the first line of /proc/stat), in nanoseconds: the busy
// share between two readings is 1 - d(idle) / d(total). The M14 wake soak
// relates late wakes to the machine's load with it.
struct SystemCpuTimes {
  std::uint64_t idle_ns = 0;
  std::uint64_t total_ns = 0;
};
[[nodiscard]] std::optional<SystemCpuTimes> ReadSystemCpuTimes();

}  // namespace se

#endif  // SE_OS_PROCESS_H_
