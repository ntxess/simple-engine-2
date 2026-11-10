// Copyright 2026 The simple-engine-2 Authors.
//
// SimThread: the dedicated sim driver's thread (docs/milestone-specs.md
// M17a; docs/design/dedicated-driver.md "Threads" (sim), "Data Flow",
// "Lifecycle"; ADR-083). One thread, role kSim, started through se_os
// StartThread, that owns every running World while it runs: the session
// is built on it (SimSessionFactory, at StartGame) and destroyed on it
// (Quit, a fault, or the thread's end), and in between it calls
// SimDriver::RunTick, unchanged, on PacedClock's absolute deadlines
// L_k = T0 + k*dt + phi.
//
// Channels (the platform side is one thread, the sim side this one):
//   commands   an SpscQueue of 256 move-only SimCommands; Send() numbers
//              them and rings the sim's Doorbell. They apply at tick
//              boundaries, in order; each snapshot header acks the last
//              applied sequence.
//   input      the InputEvent ring (4096, se_sim/input_folder.h): the
//              session's InputFolder latches each tick at its own deadline
//              (live input), unless the session plays recorded InputFrames
//              (SimSession::RecordedFrame; replays, the hash checks).
//   snapshots  a SnapshotChannel the caller owns: after each published tick
//              the sim fills a slot (the header, the session's render part,
//              the reliable section) and never waits; with no free slot it
//              skips the render part and keeps the reliable deltas (tick
//              records, audio cues, session events) for the next publish.
//              In a catch-up burst only the last tick publishes its render
//              part.
//   faults     a FaultBox: a throw out of a tick (sim_fault_boundary.cc,
//              the one catch, ADR-006) tears the session down on the sim
//              thread, posts the fault and an ended event, and the thread
//              waits for Quit.
//
// Commands (M17a's set): StartGame, Pause (stop at the next boundary),
// Resume (re-base the grid: the next tick is due now), Step{n} (while
// paused, exactly n ticks at once, then paused again), SetTimeScale,
// Suspend (no ticks until Resume), Quit (destroy the session and end the
// thread), ApplySessionEntry (M2's SessionLog, at the next boundary).
// While paused, suspended or without a session the thread waits on the
// Doorbell; Stop() (or the destructor) sends Quit, requests a stop and
// joins.

#ifndef SE_SIM_SIM_THREAD_H_
#define SE_SIM_SIM_THREAD_H_

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

#include "se_core/doorbell.h"
#include "se_core/error.h"
#include "se_core/fault_box.h"
#include "se_core/input_frame.h"
#include "se_core/snapshot.h"
#include "se_core/snapshot_channel.h"
#include "se_core/spsc_queue.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_sim/game_state.h"
#include "se_sim/input_folder.h"
#include "se_sim/paced_clock.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"

namespace se {

enum class SimOp : std::uint8_t {
  kStartGame,
  kPause,
  kResume,
  kStep,
  kSetTimeScale,
  kSuspend,
  kQuit,
  kApplySessionEntry,
};

// One platform-to-sim command. Move-only through its SessionEntry.
struct SimCommand {
  // Set by SimThread::Send: 1, 2, 3, ...
  std::uint32_t seq = 0;
  std::uint64_t stamp_ns = 0;
  SimOp op = SimOp::kPause;
  // kStep.
  std::uint32_t ticks = 0;
  // kSetTimeScale (positive, finite).
  double time_scale = 1.0;
  // kApplySessionEntry.
  SessionEntry entry;
};

inline constexpr std::uint32_t kSimCommandRingCapacity = 256;

// What the sim thread runs: built on the sim thread by the factory at
// StartGame and destroyed there. Every method runs on the sim thread,
// which holds g_sim_owner.
class SimSession {
 public:
  virtual ~SimSession() = default;
  virtual GameStateStack& States() = 0;
  virtual SimDriver& Driver() = 0;
  virtual SessionLog& Log() = 0;
  virtual ReplayRecorder& Recorder() = 0;
  virtual AudioSink& Audio() = 0;
  // A recorded InputFrame for |tick| (replays): when set, it is the tick's
  // input; nullopt latches live input from the InputEvent ring.
  [[nodiscard]] virtual std::optional<InputFrame> RecordedFrame(Tick /*tick*/) {
    return std::nullopt;
  }
  // The render part of a published slot (items, sparse index, views);
  // the default publishes none.
  virtual void FillRender(Snapshot& /*slot*/) {}
};

using SimSessionFactory =
    std::function<std::expected<std::unique_ptr<SimSession>, Error>()>;

struct SimThreadConfig {
  SimSessionFactory factory;
  PacedClockConfig clock;
  // The clock to pace with; null: the system clock (se_os's precise timer),
  // made on the sim thread. Tests pass a ManualClock. Must outlive the
  // thread.
  WaitClock* wait_clock = nullptr;
  // The channel the sim publishes into; must outlive the thread.
  SnapshotChannel* snapshots = nullptr;
  // Called on the sim thread after a slot is published, a fault is posted,
  // or the session ends (the platform's wake); may be empty.
  std::function<void()> wake_platform;
};

struct SimThreadStats {
  std::uint64_t ticks = 0;
  std::uint64_t publishes = 0;
  // Publishes whose render part was skipped for want of a free slot.
  std::uint64_t dropped_publishes = 0;
  std::uint64_t commands = 0;
  std::uint64_t bursts = 0;
  std::uint64_t dilations = 0;
};

class SimThread {
 public:
  // Starts the thread; it waits for StartGame.
  [[nodiscard]] static std::expected<std::unique_ptr<SimThread>, Error> Start(
      SimThreadConfig config);

  SimThread(const SimThread&) = delete;
  SimThread& operator=(const SimThread&) = delete;
  // Stop().
  ~SimThread();

  // Platform side: numbers |command| and queues it. False when the ring is
  // full (the caller retries; nothing is lost).
  bool Send(SimCommand command);
  // Platform side: the InputEvent ring (the sim latches from it).
  [[nodiscard]] InputEventRing& Input() noexcept { return input_; }
  // Sends Quit, requests a stop and joins. Idempotent.
  void Stop() noexcept;
  [[nodiscard]] bool Running() const noexcept;

  // Any thread.
  [[nodiscard]] const FaultBox& Fault() const noexcept { return fault_; }
  // The last command sequence the sim applied.
  [[nodiscard]] std::uint32_t AckedSeq() const noexcept {
    return acked_seq_.load(std::memory_order_acquire);
  }
  // Ticks run so far (any thread; relaxed).
  [[nodiscard]] std::uint64_t TicksRun() const noexcept {
    return ticks_run_.load(std::memory_order_acquire);
  }
  // The sim thread's counters; complete once Stop() has returned.
  [[nodiscard]] SimThreadStats Stats() const noexcept;

  struct Impl;

 private:
  explicit SimThread(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
  InputEventRing input_{kInputEventRingCapacity};
  FaultBox fault_;
  std::atomic<std::uint32_t> acked_seq_{0};
  std::atomic<std::uint64_t> ticks_run_{0};
};

}  // namespace se

#endif  // SE_SIM_SIM_THREAD_H_
