// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.time_period): the timer rule has no
// opt-out; the timer resolution is never toggled.

#include <windows.h>
#include <timeapi.h>

namespace se {

void SleepsPreciselyAnyway() {
  timeBeginPeriod(1);  // se-allow(timer): not accepted
  Sleep(1);
  timeEndPeriod(1);  // se-allow(timer): not accepted
}

}  // namespace se
