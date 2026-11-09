// Copyright 2026 The simple-engine-2 Authors.
//
// Process facts needed before SDL is initialised: the crash handler starts
// first and looks for its handler executable next to the program.

#ifndef SE_OS_PROCESS_H_
#define SE_OS_PROCESS_H_

#include <expected>
#include <string>

#include "se_core/error.h"

namespace se {

// The directory of the running executable, UTF-8, without a trailing
// separator (GetModuleFileNameW; /proc/self/exe).
[[nodiscard]] std::expected<std::string, Error> ExecutableDir();

}  // namespace se

#endif  // SE_OS_PROCESS_H_
