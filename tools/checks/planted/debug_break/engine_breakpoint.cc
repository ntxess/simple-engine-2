// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.debug_break): C++26's std::breakpoint
// stops in the debugger instead of aborting into the crash handler.

#include <debugging>

namespace se {

void BreaksWhenDebugged() { std::breakpoint_if_debugging(); }

}  // namespace se
