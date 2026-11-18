// Copyright 2026 The simple-engine-2 Authors.
//
// SimDriver: the seam between the frame loop and the simulation. RunTick
// runs one tick at a tick boundary:
//
//   GameStateStack::ApplyPending   state changes from the previous tick
//   SessionLog::ApplyDue           logged mutations due before this tick
//   ReplayRecorder::Record         the tick's InputFrame
//   Schedule::Step                 each running World
//   AudioSink::Drain               each running World (null in M2)
//
// then records the stack's hash. TickInline runs a frame's StepPlan on the
// main thread, latching one InputFrame per tick; the M17 threaded driver
// calls RunTick unchanged from its own thread. With an InputFrameRing
// attached, RunTick also writes each tick's frame into it (the crash
// dump's input history); nothing else changes. With a TickGate (M9),
// TickInline applies the stack's pending changes before each tick and
// stops the frame's ticks while the gate holds; RunTick's own
// ApplyPending then finds nothing left, so replays, which call RunTick
// directly, run the same ticks.
//
// M13: with the stack's content gate, a boundary can stop at a state that
// is not ready (GameStateStack::Waiting). CompleteBoundary finishes the
// boundary before NextTick (the stack's pending commands, then the
// SessionLog's due entries, both stopping at a wait); TickInline calls it
// before asking the TickGate, and the runtime calls it at each frame start
// while a state waits, so the push applies, and the ticks resume, as soon
// as the state is ready.

#ifndef SE_SIM_SIM_DRIVER_H_
#define SE_SIM_SIM_DRIVER_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "se_core/input_frame.h"
#include "se_core/input_frame_ring.h"
#include "se_core/jobs.h"
#include "se_core/schedule.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/time.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"

namespace se {

// Where tick sounds go: after each World's step, the World's AudioEvents
// (se_sim/audio_events.h). The runtime's AudioBridge starts them at tick
// anchor + D (M8).
class AudioSink {
 public:
  virtual ~AudioSink() = default;
  virtual void Drain(World& world, std::uint64_t tick_window_end_ns)
      SE_REQUIRES(g_sim_owner) = 0;
};

// Drops the tick's audio events (headless runs, tests).
class NullAudioSink final : public AudioSink {
 public:
  void Drain(World& world, std::uint64_t tick_window_end_ns)
      SE_REQUIRES(g_sim_owner) override;
};

// Holds a frame's remaining ticks (M9: a World never steps before its
// AssetScope is resident). TickInline applies the stack's pending changes
// and asks the gate before each tick; when it holds, the frame runs no
// more ticks (none are run late: the stepper plans afresh next frame).
class TickGate {
 public:
  virtual ~TickGate() = default;
  // True: tick |next_tick| and the rest of the frame do not run.
  virtual bool HoldTicks(const GameStateStack& states, Tick next_tick)
      SE_REQUIRES(g_sim_owner) = 0;
};

struct TickResult {
  Tick tick = 0;
  // GameStateStack::Hash().Folded() after the tick.
  std::uint64_t hash = 0;
  std::uint32_t worlds_stepped = 0;
  // The top state when the Worlds stepped.
  std::string top_state;
};

class SimDriver {
 public:
  SimDriver(Schedule& schedule, jobs::ForkJoinPool& jobs,
            Tick first_tick = 1) noexcept
      : schedule_(&schedule), jobs_(&jobs), next_tick_(first_tick) {}

  // Runs tick NextTick(); |frame| must carry that tick.
  TickResult RunTick(GameStateStack& states, const InputFrame& frame,
                     SessionLog& log, ReplayRecorder& recorder,
                     AudioSink& audio, std::uint64_t tick_window_end_ns)
      SE_REQUIRES(g_sim_owner);

  // Runs plan.steps ticks on the main thread, which takes g_sim_owner for
  // the duration (inline mode), so the caller must not hold it.
  void TickInline(GameStateStack& states, const StepPlan& plan,
                  InputSource& input, SessionLog& log, ReplayRecorder& recorder,
                  AudioSink& audio) SE_REQUIRES(g_main)
      SE_EXCLUDES(g_sim_owner);

  [[nodiscard]] Tick NextTick() const noexcept { return next_tick_; }

  // The boundary before NextTick(): the stack's pending commands, then the
  // log's entries due by NextTick(). Either stops while a state waits at
  // the content gate. RunTick's own calls then find nothing left.
  void CompleteBoundary(GameStateStack& states, SessionLog& log) const
      SE_REQUIRES(g_sim_owner);

  // TickInline asks |gate| before each tick (nullptr: none). The gate must
  // outlive its use here.
  void SetTickGate(TickGate* gate) noexcept { gate_ = gate; }
  // The last TickInline stopped at the gate.
  [[nodiscard]] bool HeldLastFrame() const noexcept { return held_; }

  // Writes every frame RunTick runs into |ring| from now on (nullptr
  // stops). The ring must outlive its use here; one registered as a dump
  // stream is never destroyed.
  void AttachInputRing(InputFrameRing* ring) noexcept { input_ring_ = ring; }
  // The results of the last TickInline call, one per tick.
  [[nodiscard]] std::span<const TickResult> FrameResults() const noexcept {
    return frame_results_;
  }

 private:
  Schedule* schedule_;
  jobs::ForkJoinPool* jobs_;
  Tick next_tick_;
  InputFrameRing* input_ring_ = nullptr;
  TickGate* gate_ = nullptr;
  bool held_ = false;
  std::vector<TickResult> frame_results_;
};

}  // namespace se

#endif  // SE_SIM_SIM_DRIVER_H_
