// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/lifecycle.h"

#include <cstdint>
#include <string_view>

namespace se {

std::string_view LifecycleStateName(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::kBooting:
      return "Booting";
    case LifecycleState::kRunning:
      return "Running";
    case LifecycleState::kModal:
      return "Modal";
    case LifecycleState::kFocusLost:
      return "FocusLost";
    case LifecycleState::kIdle:
      return "Idle";
    case LifecycleState::kSuspended:
      return "Suspended";
    case LifecycleState::kDeviceLost:
      return "DeviceLost";
    case LifecycleState::kQuitPending:
      return "QuitPending";
    case LifecycleState::kShuttingDown:
      return "ShuttingDown";
    case LifecycleState::kExited:
      return "Exited";
  }
  return "?";
}

std::string_view CallbackRateHint(CallbackRate rate) noexcept {
  switch (rate) {
    case CallbackRate::kFull:
      return "0";
    case CallbackRate::kWaitEvent:
      return "waitevent";
    case CallbackRate::kSuspended:
      return "10";
  }
  return "0";
}

void Lifecycle::BootComplete(std::uint64_t now_ns, bool focused, bool visible) {
  if (phase_ != Phase::kBooting) return;
  phase_ = Phase::kRunning;
  focused_ = focused;
  window_suspended_ = !visible;
  last_input_ns_ = now_ns;
  // The first frame starts the frame clock.
  reset_frame_clock_ = true;
  NoteState();
}

void Lifecycle::OnWindow(WindowSignal signal, std::uint64_t now_ns) {
  switch (signal) {
    case WindowSignal::kMinimized:
    case WindowSignal::kHidden:
    case WindowSignal::kOccluded:
      SetWindowSuspended(true);
      break;
    case WindowSignal::kRestored:
    case WindowSignal::kShown:
    case WindowSignal::kExposed:
    // A maximized window is visible too.
    case WindowSignal::kMaximized:
      SetWindowSuspended(false);
      break;
    case WindowSignal::kFocusGained:
      focused_ = true;
      // Input resumes from a clean held state.
      clear_held_input_ = true;
      last_input_ns_ = now_ns;
      break;
    case WindowSignal::kFocusLost:
      if (focused_) clear_held_input_ = true;
      focused_ = false;
      break;
  }
  NoteState();
}

void Lifecycle::SetModal(bool modal) {
  if (modal && !modal_) clear_held_input_ = true;
  modal_ = modal;
  NoteState();
}

void Lifecycle::OnPowerSuspend() {
  SetPowerSuspended(true);
  NoteState();
}

void Lifecycle::OnPowerResume() {
  SetPowerSuspended(false);
  NoteState();
}

void Lifecycle::OnInputActivity(std::uint64_t now_ns) {
  last_input_ns_ = now_ns;
  if (idle_) woke_ = true;
  idle_ = false;
  NoteState();
}

void Lifecycle::OnAnyEvent() {
  if (idle_) {
    idle_ = false;
    // Run at least the next frame before Idle can return.
    woke_ = true;
  }
  NoteState();
}

void Lifecycle::OnNullAcquire() { ++null_acquires_; }

void Lifecycle::OnDeviceLost() {
  device_lost_ = true;
  idle_ = false;
  NoteState();
}

void Lifecycle::OnDeviceRecovered() {
  if (device_lost_) {
    device_lost_ = false;
    // No catch-up for the time recreation took.
    reset_frame_clock_ = true;
  }
  NoteState();
}

void Lifecycle::OnSuspendProbe(bool window_visible, bool acquire_succeeded) {
  if (window_suspended_ && window_visible && acquire_succeeded) {
    SetWindowSuspended(false);
    NoteState();
  }
}

void Lifecycle::Update(std::uint64_t now_ns) {
  const bool can_idle = config_.idle_allowed && phase_ == Phase::kRunning &&
                        !Suspended() && !modal_ && !device_lost_;
  if (!can_idle) {
    idle_ = false;
  } else if (woke_) {
    woke_ = false;
  } else if (!idle_ && now_ns >= last_input_ns_ &&
             now_ns - last_input_ns_ >= config_.idle_after_ns) {
    idle_ = true;
  }
  NoteState();
}

void Lifecycle::OnQuitPending() {
  if (phase_ == Phase::kRunning) phase_ = Phase::kQuitPending;
  NoteState();
}

void Lifecycle::OnQuitCancelled() {
  if (phase_ == Phase::kQuitPending) phase_ = Phase::kRunning;
  NoteState();
}

void Lifecycle::OnShutdownStarted() {
  if (phase_ != Phase::kExited) phase_ = Phase::kShuttingDown;
  NoteState();
}

void Lifecycle::OnExited() {
  phase_ = Phase::kExited;
  NoteState();
}

LifecycleState Lifecycle::State() const noexcept {
  switch (phase_) {
    case Phase::kBooting:
      return LifecycleState::kBooting;
    case Phase::kShuttingDown:
      return LifecycleState::kShuttingDown;
    case Phase::kExited:
      return LifecycleState::kExited;
    case Phase::kQuitPending:
      return LifecycleState::kQuitPending;
    case Phase::kRunning:
      break;
  }
  if (device_lost_) return LifecycleState::kDeviceLost;
  if (Suspended()) return LifecycleState::kSuspended;
  if (modal_) return LifecycleState::kModal;
  if (idle_) return LifecycleState::kIdle;
  if (!focused_) return LifecycleState::kFocusLost;
  return LifecycleState::kRunning;
}

CallbackRate Lifecycle::Rate() const noexcept {
  if (phase_ == Phase::kBooting || phase_ == Phase::kShuttingDown ||
      phase_ == Phase::kExited) {
    return CallbackRate::kFull;
  }
  // A draining quit keeps iterating, at 10 Hz while nothing is visible.
  // A device recreation that failed is retried at 10 Hz too.
  if (Suspended() || device_lost_) return CallbackRate::kSuspended;
  if (phase_ == Phase::kRunning && idle_) return CallbackRate::kWaitEvent;
  return CallbackRate::kFull;
}

bool Lifecycle::ShouldRender() const noexcept {
  if (phase_ != Phase::kRunning && phase_ != Phase::kQuitPending) return false;
  return !Suspended() && !device_lost_;
}

bool Lifecycle::GameplayRuns() const noexcept {
  if (phase_ != Phase::kRunning && phase_ != Phase::kQuitPending) return false;
  if (Suspended() || device_lost_) return false;
  if (!focused_ && config_.pause_on_focus_lost) return false;
  return true;
}

bool Lifecycle::TakeClearHeldInput() noexcept {
  const bool take = clear_held_input_;
  clear_held_input_ = false;
  return take;
}

bool Lifecycle::TakeFrameClockReset() noexcept {
  const bool take = reset_frame_clock_;
  reset_frame_clock_ = false;
  return take;
}

void Lifecycle::SetWindowSuspended(bool suspended) {
  const bool was = Suspended();
  window_suspended_ = suspended;
  AfterSuspendChange(was);
}

void Lifecycle::SetPowerSuspended(bool suspended) {
  const bool was = Suspended();
  power_suspended_ = suspended;
  AfterSuspendChange(was);
}

void Lifecycle::AfterSuspendChange(bool was_suspended) {
  if (was_suspended && !Suspended()) {
    // No catch-up: the paused time is not owed as ticks.
    reset_frame_clock_ = true;
  } else if (!was_suspended && Suspended()) {
    idle_ = false;
  }
}

void Lifecycle::NoteState() {
  const LifecycleState state = State();
  if (state != last_state_) {
    ++transitions_;
    last_state_ = state;
  }
}

}  // namespace se
