// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.dump_write): engine code writes a
// minidump in process instead of leaving it to crashpad's handler.

#include <windows.h>
#include <dbghelp.h>

namespace se {

void WritesItsOwnDump(HANDLE file) {
  MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                    MiniDumpNormal, nullptr, nullptr, nullptr);
}

}  // namespace se
