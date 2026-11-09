// Planted violation: engine code writing a minidump in process. Must be
// detected: the crash handler writes every dump out of process (M4b).
#include <windows.h>

#include <dbghelp.h>

bool WriteDump(HANDLE file) {
  return MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                           MiniDumpNormal, nullptr, nullptr, nullptr) != 0;
}
