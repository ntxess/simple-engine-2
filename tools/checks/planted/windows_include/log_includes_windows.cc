// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.windows_include): a module outside
// se_os, se_crash, se_platform and se_render includes <windows.h>.

#include <windows.h>

namespace se {

void CallsWin32Directly() { OutputDebugStringA("platform code elsewhere"); }

}  // namespace se
