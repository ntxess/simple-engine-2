// Copyright 2026 The simple-engine-2 Authors.
//
// The application lifecycle as a pure state machine (docs/design/
// mt-design.md "Lifecycle"; docs/milestone-specs.md M5, lifecycle.h). No
// SDL: the App feeds it window events, the Modal flag, power messages,
// input activity, quit progress and the clock, and reads back what the
// frame should do. Unit-tested without a window (tests/platform).
//
// States (M5): Booting, Running, Modal, FocusLost, Idle, Suspended,
// QuitPending, ShuttingDown, Exited. Overloaded, HalfRate, DeviceLost, Hung
// and the Play states arrive with M6, M7, M10 and M12.
//
// Conditions are tracked separately and the state is the dominant one:
//   Booting, Exited, ShuttingDown   the phase
//   QuitPending                     a quit is draining (two-phase quit)
//   Suspended                       minimized, hidden or occluded, or a
//                                   power suspend; never a NULL acquire
//   Modal                           a Windows modal loop (drag, resize,
//                                   menu) is running
//   Idle                            allowed (the editor, M10), and no input
//                                   for idle_after_ns
//   FocusLost                       visible but unfocused
//   Running                         otherwise
//
// Rules from the design:
//   - Suspended is entered only on MINIMIZED, HIDDEN or OCCLUDED or a power
//     suspend. It is left on RESTORED, SHOWN or EXPOSED, on a successful
//     probe while the window is visible again, or on power resume (each
//     source clears its own condition). Leaving it resets the frame clock:
//     no catch-up ticks.
//   - FocusLost and Modal clear held input when entered (SDL resets the
//     keyboard during a modal loop).
//   - Idle ends on any event, including a wake; the frame after a wake
//     always runs before Idle can return.
//   - Modal removes waits and is never Overloaded (M6 reads Modal()).
//
// Outputs: the callback rate (SDL_HINT_MAIN_CALLBACK_RATE "0", "waitevent"
// when Idle, "10" when Suspended), whether to render, whether gameplay
// runs, and two one-shot requests: clear held input, reset the frame clock.

#ifndef SE_RUNTIME_LIFECYCLE_H_
#define SE_RUNTIME_LIFECYCLE_H_

#include <cstdint>
#include <string_view>

namespace se {

enum class LifecycleState : std::uint8_t {
  kBooting,
  kRunning,
  kModal,
  kFocusLost,
  kIdle,
  kSuspended,
  kQuitPending,
  kShuttingDown,
  kExited,
};

[[nodiscard]] std::string_view LifecycleStateName(
    LifecycleState state) noexcept;

enum class CallbackRate : std::uint8_t {
  // "0": as fast as presentation allows.
  kFull,
  // "waitevent": iterate only after an event (Idle).
  kWaitEvent,
  // "10": ten iterates per second (Suspended).
  kSuspended,
};

// The SDL_HINT_MAIN_CALLBACK_RATE value for |rate|.
[[nodiscard]] std::string_view CallbackRateHint(CallbackRate rate) noexcept;

// The window events the lifecycle reacts to.
enum class WindowSignal : std::uint8_t {
  kShown,
  kHidden,
  kExposed,
  kOccluded,
  kMinimized,
  kMaximized,
  kRestored,
  kFocusGained,
  kFocusLost,
};

struct LifecycleConfig {
  // Idle is allowed (the editor in Edit mode, M10); games keep running.
  bool idle_allowed = false;
  std::uint64_t idle_after_ns = 1'000'000'000;
  // Gameplay pauses while unfocused (a game's Pause state arrives in M13).
  bool pause_on_focus_lost = false;
};

class Lifecycle {
 public:
  Lifecycle() = default;
  explicit Lifecycle(const LifecycleConfig& config) : config_(config) {}

  // Booting -> Running, with the window's state at the end of boot.
  void BootComplete(std::uint64_t now_ns, bool focused, bool visible);

  void OnWindow(WindowSignal signal, std::uint64_t now_ns);
  void SetModal(bool modal);
  void OnPowerSuspend();
  void OnPowerResume();
  // Keyboard, mouse, gamepad or text input: restarts the idle timer.
  void OnInputActivity(std::uint64_t now_ns);
  // Any SDL event, including a wake: leaves Idle.
  void OnAnyEvent();
  // A NULL swapchain texture: counted, never a state change.
  void OnNullAcquire();
  // The 10 Hz check while Suspended: the window flags show it neither
  // minimized nor hidden and a probe acquire returned a texture.
  void OnSuspendProbe(bool window_visible, bool acquire_succeeded);
  // Once per iterate, before the frame: time-based transitions (Idle).
  void Update(std::uint64_t now_ns);

  // Two-phase quit progress (QuitController).
  void OnQuitPending();
  void OnQuitCancelled();
  void OnShutdownStarted();
  void OnExited();

  [[nodiscard]] LifecycleState State() const noexcept;
  [[nodiscard]] CallbackRate Rate() const noexcept;
  [[nodiscard]] bool ShouldRender() const noexcept;
  [[nodiscard]] bool GameplayRuns() const noexcept;
  [[nodiscard]] bool Modal() const noexcept { return modal_; }
  [[nodiscard]] bool Suspended() const noexcept {
    return window_suspended_ || power_suspended_;
  }
  // The two sources of Suspended: the window (minimized, hidden, occluded)
  // and a power suspend.
  [[nodiscard]] bool WindowSuspended() const noexcept {
    return window_suspended_;
  }
  [[nodiscard]] bool PowerSuspended() const noexcept {
    return power_suspended_;
  }
  [[nodiscard]] bool Focused() const noexcept { return focused_; }

  // One-shot outputs: true once after the change that needs them.
  [[nodiscard]] bool TakeClearHeldInput() noexcept;
  [[nodiscard]] bool TakeFrameClockReset() noexcept;

  [[nodiscard]] std::uint64_t NullAcquires() const noexcept {
    return null_acquires_;
  }
  // State changes so far (State() differing from the previous Update or
  // event), for logs and tests.
  [[nodiscard]] std::uint32_t Transitions() const noexcept {
    return transitions_;
  }

 private:
  enum class Phase : std::uint8_t {
    kBooting,
    kRunning,
    kQuitPending,
    kShuttingDown,
    kExited,
  };

  void SetWindowSuspended(bool suspended);
  void SetPowerSuspended(bool suspended);
  void AfterSuspendChange(bool was_suspended);
  void NoteState();

  LifecycleConfig config_;
  Phase phase_ = Phase::kBooting;
  bool focused_ = true;
  bool window_suspended_ = false;
  bool power_suspended_ = false;
  bool modal_ = false;
  bool idle_ = false;
  bool woke_ = false;
  std::uint64_t last_input_ns_ = 0;
  bool clear_held_input_ = false;
  bool reset_frame_clock_ = false;
  std::uint64_t null_acquires_ = 0;
  LifecycleState last_state_ = LifecycleState::kBooting;
  std::uint32_t transitions_ = 0;
};

}  // namespace se

#endif  // SE_RUNTIME_LIFECYCLE_H_
