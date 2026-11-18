// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.debug_break): engine code breaks into
// the debugger instead of aborting into the crash handler.

namespace se {

void StopsInTheDebugger() { __debugbreak(); }

}  // namespace se
