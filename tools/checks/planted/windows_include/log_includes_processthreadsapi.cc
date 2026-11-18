// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.windows_include): a Win32 API family
// header, included directly.

#include <processthreadsapi.h>

namespace se {

unsigned long ReadsItsThreadId() { return GetCurrentThreadId(); }

}  // namespace se
