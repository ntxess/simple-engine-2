// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/sim_driver.h"

#include <cstdint>
#include <string>
#include <vector>

#include "se_core/assert.h"
#include "se_core/input_frame.h"
#include "se_core/thread_role.h"
#include "se_core/time.h"
#include "se_core/world.h"

namespace se {

TickResult SimDriver::RunTick(GameStateStack& states, const InputFrame& frame,
                              SessionLog& log, ReplayRecorder& recorder,
                              AudioSink& audio,
                              std::uint64_t tick_window_end_ns) {
  SE_ASSERT_MSG(frame.tick == next_tick_,
                "RunTick needs the InputFrame of the next tick");
  const Tick tick = next_tick_;
  states.ApplyPending();
  log.ApplyDue(tick, states);
  recorder.Record(frame);
  if (input_ring_ != nullptr) input_ring_->Write(frame);

  TickResult result;
  result.tick = tick;
  result.top_state = std::string(states.TopId());
  for (World* world : states.RunningWorlds()) {
    schedule_->Step(*world, frame, *jobs_);
    audio.Drain(*world, tick_window_end_ns);
    ++result.worlds_stepped;
  }
  result.hash = states.Hash().Folded();
  recorder.RecordHash(tick, result.hash);
  ++next_tick_;
  return result;
}

void SimDriver::TickInline(GameStateStack& states, const StepPlan& plan,
                           InputSource& input, SessionLog& log,
                           ReplayRecorder& recorder, AudioSink& audio) {
  SE_ASSERT_MAIN_THREAD();
  SE_ASSERT_MSG(plan.steps >= 0 && plan.steps <= kMaxStepsPerFrame,
                "a frame runs at most kMaxStepsPerFrame ticks");
  // Inline mode: the main thread owns the Worlds while it steps them.
  const InlineOwnerScope owner(OnMainThread());
  frame_results_.clear();
  for (int i = 0; i < plan.steps; ++i) {
    const std::uint64_t window_end =
        plan.tick_window_end_ns[static_cast<std::size_t>(i)];
    const InputFrame frame =
        input.Latch(next_tick_, window_end, i + 1 == plan.steps);
    frame_results_.push_back(
        RunTick(states, frame, log, recorder, audio, window_end));
  }
}

}  // namespace se
