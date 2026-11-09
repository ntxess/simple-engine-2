// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.debug_break): the debugbreak rule has no
// opt-out ("never DebugBreak").

namespace se {

void StopsInTheDebuggerAnyway() {
  __debugbreak();  // se-allow(debugbreak): not accepted
}

}  // namespace se
