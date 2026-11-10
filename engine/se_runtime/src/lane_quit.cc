// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/lane_quit.h"

#include <cstdint>

#include "se_core/log.h"
#include "se_runtime/quit.h"

namespace se {

QuitPoll LaneQuitProvider::PollQuit(QuitReason /*reason*/) {
  const std::uint64_t failures = lane_->DurableFailures();
  if (!polling_) {
    polling_ = true;
    failures_at_start_ = failures;
  }
  if (failures > failures_at_start_) {
    polling_ = false;
    SE_LOG_WARN("quit cancelled: {} Durable job(s) failed during the quit",
                failures - failures_at_start_);
    return QuitPoll::kCancel;
  }
  if (lane_->DurablePending() > 0) return QuitPoll::kPending;
  polling_ = false;
  return QuitPoll::kReady;
}

}  // namespace se
