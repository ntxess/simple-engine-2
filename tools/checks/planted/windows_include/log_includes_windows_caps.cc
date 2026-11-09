// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.windows_include): <windows.h> in another
// letter case is the same header on Windows.

#include <WINDOWS.H>

namespace se {

void CallsWin32InCapitals() { OutputDebugStringA("platform code elsewhere"); }

}  // namespace se
