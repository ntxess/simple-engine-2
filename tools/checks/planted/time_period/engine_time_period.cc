// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.time_period): the timer resolution is
// toggled around a wait.

#include <windows.h>
#include <timeapi.h>

namespace se {

void SleepsPrecisely() {
  timeBeginPeriod(1);
  Sleep(1);
  timeEndPeriod(1);
}

}  // namespace se
