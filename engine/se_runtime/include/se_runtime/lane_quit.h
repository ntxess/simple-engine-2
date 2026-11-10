// Copyright 2026 The simple-engine-2 Authors.
//
// The background lane's part in the two-phase quit (docs/milestone-specs.md
// M9; docs/design/mt-design.md "Shutdown Order" step 1): Durable jobs are
// never cancelled and quit waits for them. LaneQuitProvider reports
// kPending while the lane has Durable jobs running or queued, and kCancel
// when a Durable job failed during this quit (a failed save keeps its
// document dirty, and the quit is called off). It logs when a quit starts
// to wait for Durable jobs and when they are done (ADR-089). Main thread.

#ifndef SE_RUNTIME_LANE_QUIT_H_
#define SE_RUNTIME_LANE_QUIT_H_

#include <cstdint>
#include <string_view>

#include "se_assets/background_lane.h"
#include "se_runtime/quit.h"

namespace se {

class LaneQuitProvider final : public QuitProvider {
 public:
  explicit LaneQuitProvider(const BackgroundLane& lane) : lane_(&lane) {}

  QuitPoll PollQuit(QuitReason reason) override;
  [[nodiscard]] std::string_view Name() const override {
    return "background lane (Durable jobs)";
  }

 private:
  const BackgroundLane* lane_;
  // The lane's Durable failures when this quit's polls began.
  bool polling_ = false;
  bool waiting_logged_ = false;
  std::uint64_t failures_at_start_ = 0;
};

}  // namespace se

#endif  // SE_RUNTIME_LANE_QUIT_H_
