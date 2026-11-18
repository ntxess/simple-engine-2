// Copyright 2026 The simple-engine-2 Authors.
//
// The in-driver flag (docs/milestone-specs.md M12; docs/design/mt-design.md
// "watchdog"): main raises it around SDL_GPU's swapchain wait and acquire,
// the submit that presents, its fence waits and SDL_WaitForGPUIdle
// (se_render), and the FramePacer's frame-latency, fence and swapchain
// waits (se_runtime), so that the watchdog labels a hang that finds main
// inside one a GPU or compositor stall, in the dump's reason and in the
// log. It never excuses a hang.
//
//   {
//     const se::DriverWaitScope in_driver;
//     SDL_WaitForGPUSwapchain(device, window);
//   }
//
// Main only (every SDL_GPU call runs there); scopes nest. The watchdog
// reads the flag with an acquire load through WatchdogInputs::in_driver.

#ifndef SE_PLATFORM_DRIVER_WAIT_H_
#define SE_PLATFORM_DRIVER_WAIT_H_

#include <atomic>

namespace se {

// The process's in-driver flag.
[[nodiscard]] const std::atomic<bool>& DriverWaitFlag() noexcept;

// True while main is inside a DriverWaitScope.
[[nodiscard]] bool InDriverWait() noexcept;

// Raises the flag for its lifetime and restores what it found.
class DriverWaitScope {
 public:
  DriverWaitScope() noexcept;
  ~DriverWaitScope();
  DriverWaitScope(const DriverWaitScope&) = delete;
  DriverWaitScope& operator=(const DriverWaitScope&) = delete;

 private:
  bool previous_;
};

}  // namespace se

#endif  // SE_PLATFORM_DRIVER_WAIT_H_
