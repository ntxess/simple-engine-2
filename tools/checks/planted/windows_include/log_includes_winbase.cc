// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.windows_include): one of the headers
// <windows.h> gathers, included directly.

#include <winbase.h>

namespace se {

void CallsWinBase() { OutputDebugStringA("platform code elsewhere"); }

}  // namespace se
