// Copyright 2026 The simple-engine-2 Authors.
//
// Two-phase quit (docs/design/mt-design.md change 7, "Shutdown Order" steps
// 1-3; docs/milestone-specs.md M5, quit.h). Pure: no SDL, unit-tested.
//
// SDL_EVENT_QUIT, a window close, App::RequestQuit and --frames call
// Request(): the quit becomes pending and the callbacks keep returning
// CONTINUE. Each iterate calls Poll(), which asks every QuitProvider:
//   kPending  work is still draining (M9's Durable jobs, M10's prompt and
//             dialogs); ask again next iterate;
//   kCancel   the quit is called off (the user cancelled, a Durable save
//             failed); the app runs on;
//   kReady    nothing left.
// ReadyToExit() turns true only after a Poll in which every provider was
// ready: the drain is over, the next iterate returns SDL_APP_SUCCESS, and
// the shutdown deadline starts then (the deadline hook; the watchdog's 5 s
// deadline is wired at integration).
//
// WM_QUERYENDSESSION, WM_ENDSESSION and SDL_EVENT_TERMINATING call
// RequestImmediate(): the synchronous recovery hook runs first, on the
// calling (main) thread, before the call returns; then the app is ready to
// exit at once, without a drain (the OS may end the process at any time).

#ifndef SE_RUNTIME_QUIT_H_
#define SE_RUNTIME_QUIT_H_

#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace se {

enum class QuitReason : std::uint8_t {
  kNone,
  kQuitEvent,
  kWindowClose,
  kRequested,
  kFrames,
  kSessionEnd,
  kTerminating,
  kFault,
};

[[nodiscard]] std::string_view QuitReasonName(QuitReason reason) noexcept;

enum class QuitPoll : std::uint8_t { kReady, kPending, kCancel };

class QuitProvider {
 public:
  virtual ~QuitProvider() = default;
  // Called once per iterate while a quit is pending.
  virtual QuitPoll PollQuit(QuitReason reason) = 0;
  [[nodiscard]] virtual std::string_view Name() const = 0;
};

class QuitController {
 public:
  // Runs synchronously on WM_QUERYENDSESSION and TERMINATING: write what
  // must survive (M10's dirty documents) to recovery files.
  using RecoveryHook = std::function<void(QuitReason reason)>;
  // Called once, when the drain ends (the shutdown deadline starts).
  using DeadlineHook = std::function<void()>;

  void AddProvider(QuitProvider* provider);
  void RemoveProvider(QuitProvider* provider);
  void SetRecoveryHook(RecoveryHook hook) { recovery_ = std::move(hook); }
  void SetDeadlineHook(DeadlineHook hook) { deadline_ = std::move(hook); }

  // Starts a two-phase quit (idempotent while one is pending).
  void Request(QuitReason reason);
  // Session end or TERMINATING: the recovery hook, then ready at once. The
  // hook runs once per quit.
  void RequestImmediate(QuitReason reason);
  // Once per iterate: polls the providers while a quit is pending.
  void Poll();

  [[nodiscard]] bool Pending() const noexcept { return pending_; }
  [[nodiscard]] bool ReadyToExit() const noexcept { return ready_; }
  [[nodiscard]] QuitReason Reason() const noexcept { return reason_; }
  [[nodiscard]] bool DeadlineStarted() const noexcept {
    return deadline_started_;
  }
  [[nodiscard]] std::uint32_t Cancellations() const noexcept {
    return cancellations_;
  }
  [[nodiscard]] std::uint32_t RecoveryRuns() const noexcept {
    return recovery_runs_;
  }
  // Polls while pending, for logs and tests.
  [[nodiscard]] std::uint32_t PendingPolls() const noexcept {
    return pending_polls_;
  }
  // The provider that last kept the quit pending or cancelled it.
  [[nodiscard]] std::string_view Blocker() const noexcept { return blocker_; }

 private:
  void BecomeReady();

  std::vector<QuitProvider*> providers_;
  RecoveryHook recovery_;
  DeadlineHook deadline_;
  bool pending_ = false;
  bool ready_ = false;
  bool recovered_ = false;
  bool deadline_started_ = false;
  QuitReason reason_ = QuitReason::kNone;
  std::uint32_t cancellations_ = 0;
  std::uint32_t recovery_runs_ = 0;
  std::uint32_t pending_polls_ = 0;
  std::string_view blocker_;
};

}  // namespace se

#endif  // SE_RUNTIME_QUIT_H_
