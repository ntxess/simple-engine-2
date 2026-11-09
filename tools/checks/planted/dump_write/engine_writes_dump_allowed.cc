// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.dump_write, checks.planted.dump_header):
// the dump and dump_header rules have no opt-out; crashpad writes every
// dump out of process.

#include <windows.h>
#include <DbgHelp.h>  // se-allow(dump_header): not accepted

namespace se {

void WritesItsOwnDumpAnyway(HANDLE file) {
  MiniDumpWriteDump(  // se-allow(dump): not accepted
      GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
      nullptr, nullptr, nullptr);
}

}  // namespace se
