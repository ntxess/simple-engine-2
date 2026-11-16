// Copyright 2026 The simple-engine-2 Authors.
//
// The --hang-test hook (test builds; docs/milestone-specs.md M12): main
// loops inside a system at tick N, so that the watchdog declares a hang
// and its dump shows main's breadcrumb in that system at that tick.
//
//   const GameModule* module = HangTestModule(setup.module, config.hang_test);
//   ModuleInstance instance(*module);  // the game's systems, then HangTest
//
// HangTest runs last in the Cleanup phase of every World stepped, and at the
// tick of the frame's InputFrame equal to after_ticks it loops (1 ms sleeps)
// forever, or for for_ms milliseconds on the unbiased clock; with in_driver
// it loops inside a DriverWaitScope, as main would in a stalled SDL_GPU
// wait. It fires once per App.

#ifndef ENGINE_SE_RUNTIME_SRC_HANG_TEST_H_
#define ENGINE_SE_RUNTIME_SRC_HANG_TEST_H_

#include "se_runtime/app_config.h"
#include "se_sim/game_module.h"

namespace se {

// |module| with HangTest appended to its systems when |hook| is on, else
// |module| itself. The returned module and the hook's state live until the
// next call: one App at a time, on main.
[[nodiscard]] const GameModule* HangTestModule(const GameModule* module,
                                               const HangTestHook& hook);

}  // namespace se

#endif  // ENGINE_SE_RUNTIME_SRC_HANG_TEST_H_
