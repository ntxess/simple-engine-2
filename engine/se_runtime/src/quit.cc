// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/quit.h"

#include <algorithm>
#include <string_view>

namespace se {

std::string_view QuitReasonName(QuitReason reason) noexcept {
  switch (reason) {
    case QuitReason::kNone:
      return "none";
    case QuitReason::kQuitEvent:
      return "quit event";
    case QuitReason::kWindowClose:
      return "window close";
    case QuitReason::kRequested:
      return "requested";
    case QuitReason::kFrames:
      return "--frames reached";
    case QuitReason::kSessionEnd:
      return "session end";
    case QuitReason::kTerminating:
      return "terminating";
    case QuitReason::kFault:
      return "fault";
  }
  return "?";
}

void QuitController::AddProvider(QuitProvider* provider) {
  if (std::ranges::find(providers_, provider) == providers_.end()) {
    providers_.push_back(provider);
  }
}

void QuitController::RemoveProvider(QuitProvider* provider) {
  std::erase(providers_, provider);
}

void QuitController::Request(QuitReason reason) {
  if (ready_ || pending_) return;
  pending_ = true;
  reason_ = reason;
}

void QuitController::RequestImmediate(QuitReason reason) {
  if (!recovered_) {
    recovered_ = true;
    ++recovery_runs_;
    if (recovery_) recovery_(reason);
  }
  if (ready_) return;
  reason_ = reason;
  pending_ = false;
  BecomeReady();
}

void QuitController::Poll() {
  if (!pending_ || ready_) return;
  ++pending_polls_;
  bool all_ready = true;
  for (QuitProvider* provider : providers_) {
    const QuitPoll poll = provider->PollQuit(reason_);
    if (poll == QuitPoll::kCancel) {
      blocker_ = provider->Name();
      pending_ = false;
      reason_ = QuitReason::kNone;
      ++cancellations_;
      for (QuitProvider* each : providers_) each->QuitCancelled();
      return;
    }
    if (poll == QuitPoll::kPending && all_ready) {
      blocker_ = provider->Name();
      all_ready = false;
    }
  }
  if (!all_ready) return;
  pending_ = false;
  BecomeReady();
}

void QuitController::BecomeReady() {
  ready_ = true;
  if (!deadline_started_) {
    deadline_started_ = true;
    if (deadline_) deadline_();
  }
}

}  // namespace se
