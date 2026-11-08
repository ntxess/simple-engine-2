// Copyright 2026 The simple-engine-2 Authors.
//
// se_core declares WakeMain() and se_platform defines it with an SDL user
// event. Headless test and tool binaries have no event loop to wake, so they
// link this null definition.

#include "se_core/thread_role.h"

namespace se {

void WakeMain() noexcept {}

}  // namespace se
