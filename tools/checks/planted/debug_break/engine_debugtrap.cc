// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.debug_break): clang's debug trap breaks
// into the debugger like __debugbreak.

namespace se {

void TrapsIntoTheDebugger() { __builtin_debugtrap(); }

}  // namespace se
