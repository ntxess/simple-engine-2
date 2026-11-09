// Copyright 2026 The simple-engine-2 Authors.
//
// Whether a debugger is attached: the watchdog does not declare a hang
// while one is, and the FixedStepper allows longer stalls.

#ifndef SE_OS_DEBUGGER_H_
#define SE_OS_DEBUGGER_H_

namespace se {

// IsDebuggerPresent on Windows; a non-zero TracerPid in /proc/self/status
// on Linux.
[[nodiscard]] bool DebuggerAttached() noexcept;

}  // namespace se

#endif  // SE_OS_DEBUGGER_H_
