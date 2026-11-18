// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/lane_quit.h"

#include <algorithm>
#include <cstdint>

#include "se_core/log.h"
#include "se_runtime/quit.h"

namespace se {

QuitPoll LaneQuitProvider::PollQuit(QuitReason /*reason*/) {
  const std::uint64_t failures = lane_->DurableFailures();
  if (!polling_) {
    polling_ = true;
    waiting_logged_ = false;
    failures_at_start_ = failures;
  }
  if (failures > failures_at_start_ && cancel_on_failure_) {
    polling_ = false;
    SE_LOG_WARN("quit cancelled: {} Durable job(s) failed during the quit",
                failures - failures_at_start_);
    return QuitPoll::kCancel;
  }
  if (failures > failures_at_start_ && failures > failures_logged_) {
    // The game build: logged, and the quit goes on (M13 decision 5).
    SE_LOG_ERROR(
        "{} Durable job(s) failed during the quit; the game's quit goes on "
        "(what they wrote is lost)",
        failures - std::max(failures_at_start_, failures_logged_));
    failures_logged_ = failures;
  }
  if (const std::uint32_t pending = lane_->DurablePending(); pending > 0) {
    // Logged once per quit (ADR-089).
    if (!waiting_logged_) {
      waiting_logged_ = true;
      SE_LOG_INFO("quit waits for {} Durable lane job(s)", pending);
    }
    return QuitPoll::kPending;
  }
  if (waiting_logged_) SE_LOG_INFO("the lane's Durable jobs are done");
  polling_ = false;
  waiting_logged_ = false;
  return QuitPoll::kReady;
}

void LaneQuitProvider::QuitCancelled() {
  polling_ = false;
  waiting_logged_ = false;
}

}  // namespace se
