// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/sim_thread.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include "se_core/breadcrumb.h"
#include "se_core/error.h"
#include "se_core/fault_box.h"
#include "se_core/log.h"
#include "se_core/snapshot.h"
#include "se_core/thread_role.h"
#include "se_os/thread.h"
#include "se_sim/paced_clock.h"
#include "src/sim_fault_boundary.h"

namespace se {

struct SimThread::Impl {
  explicit Impl(SimThreadConfig thread_config)
      : config(std::move(thread_config)) {}

  // ---- Platform side ----
  SimThreadConfig config;
  SpscQueue<SimCommand> commands{kSimCommandRingCapacity};
  Doorbell bell;
  std::uint32_t next_seq = 0;
  Thread thread;
  bool stopped = false;

  // ---- Shared with SimThread (set once before the thread starts) ----
  InputEventRing* input = nullptr;
  FaultBox* fault = nullptr;
  std::atomic<std::uint32_t>* acked = nullptr;
  std::atomic<std::uint64_t>* ticks_run = nullptr;

  // ---- Sim side ----
  std::unique_ptr<WaitClock> own_clock;
  WaitClock* wait_clock = nullptr;
  std::unique_ptr<PacedClock> clock;
  std::unique_ptr<SimSession> session;
  InputFolder folder;
  std::uint64_t session_id = 0;
  std::uint32_t generation = 0;
  // The session's GameStateStack::Revision() the generation stands for.
  std::uint32_t stack_revision = 0;
  SimRunState state = SimRunState::kPaused;
  bool quit = false;
  bool faulted = false;
  std::uint32_t steps_left = 0;
  std::uint32_t acked_seq = 0;
  // Reliable deltas since the last successful publish.
  ReliableDeltas pending;
  std::uint32_t dropped = 0;
  // The spin cap's warning was logged (PacedClock does not log).
  bool cap_warned = false;
  SimThreadStats stats;

  void Run(const std::stop_token& stop) SE_REQUIRES(g_sim_owner);
  void DrainCommands() SE_REQUIRES(g_sim_owner);
  void Apply(SimCommand& command) SE_REQUIRES(g_sim_owner);
  void StartGame(std::uint64_t now_ns) SE_REQUIRES(g_sim_owner);
  void EndSession(SessionEventKind kind) SE_REQUIRES(g_sim_owner);
  // Runs tick |tick| latched at |latch_ns|; publishes after it. False when
  // the tick faulted (the session is gone then).
  bool RunOneTick(Tick tick, std::uint64_t latch_ns, bool render)
      SE_REQUIRES(g_sim_owner);
  void Event(SessionEventKind kind, std::uint64_t value = 0)
      SE_REQUIRES(g_sim_owner);
  // False when no slot was free (the deltas stay pending).
  bool Publish(Tick tick, std::uint64_t latch_ns, std::uint64_t tick_cost_ns,
               std::uint64_t hash, bool render) SE_REQUIRES(g_sim_owner);
  void Wake() const {
    if (config.wake_platform) config.wake_platform();
  }
};

void SimThread::Impl::Event(SessionEventKind kind, std::uint64_t value) {
  SessionEvent event;
  event.kind = kind;
  event.session = session_id;
  event.generation = generation;
  event.tick = session ? session->Driver().NextTick() : 0;
  event.value = value;
  pending.events.push_back(event);
}

void SimThread::Impl::StartGame(std::uint64_t now_ns) {
  if (session) EndSession(SessionEventKind::kEnded);
  ++session_id;
  ++generation;
  folder.Reset();
  std::expected<std::unique_ptr<SimSession>, Error> made =
      Fail(ErrorCode::kInvalidArgument, "no session factory");
  if (config.factory) made = config.factory();
  if (!made) {
    SE_LOG_ERROR("sim: the session did not start: {}", made.error().message);
    Event(SessionEventKind::kStartFailed,
          static_cast<std::uint64_t>(made.error().code));
    Publish(0, now_ns, 0, 0, /*render=*/false);
    return;
  }
  session = std::move(*made);
  stack_revision = session->States().Revision();
  faulted = false;
  state = SimRunState::kRunning;
  steps_left = 0;
  // The next tick is due now: the grid starts here.
  clock->Start(session->Driver().NextTick(), now_ns);
  Event(SessionEventKind::kStarted);
}

void SimThread::Impl::EndSession(SessionEventKind kind) {
  if (!session) return;
  Event(kind);
  // The Worlds die on their owner thread.
  session.reset();
  state = SimRunState::kPaused;
  steps_left = 0;
}

void SimThread::Impl::Apply(SimCommand& command) {
  ++stats.commands;
  const std::uint64_t now = wait_clock->NowNs();
  switch (command.op) {
    case SimOp::kStartGame:
      StartGame(now);
      break;
    case SimOp::kPause:
      if (session && state == SimRunState::kRunning) {
        state = SimRunState::kPaused;
        Event(SessionEventKind::kPaused);
      }
      break;
    case SimOp::kResume:
      if (session && state != SimRunState::kRunning) {
        state = SimRunState::kRunning;
        steps_left = 0;
        // Resume re-bases the grid: the next tick is due now.
        clock->Rebase(session->Driver().NextTick(), now);
        Event(SessionEventKind::kResumed);
      }
      break;
    case SimOp::kStep:
      // Steps only while paused: exactly n ticks, then paused again.
      if (session && state == SimRunState::kPaused) {
        steps_left += command.ticks;
      }
      break;
    case SimOp::kSetTimeScale:
      // Positive and finite (a NaN fails both comparisons).
      if (session && command.time_scale > 0.0 && command.time_scale < 1e9) {
        clock->SetTimeScale(command.time_scale, session->Driver().NextTick());
      }
      break;
    case SimOp::kSuspend:
      if (session && state != SimRunState::kSuspended) {
        state = SimRunState::kSuspended;
        steps_left = 0;
        Event(SessionEventKind::kSuspended);
      }
      break;
    case SimOp::kQuit:
      quit = true;
      break;
    case SimOp::kApplySessionEntry:
      if (session) {
        session->Log().Enqueue(std::move(command.entry),
                               session->Driver().NextTick(), session->States());
      }
      break;
  }
  acked_seq = command.seq;
  acked->store(command.seq, std::memory_order_release);
}

void SimThread::Impl::DrainCommands() {
  while (SimCommand* command = commands.Front()) {
    Apply(*command);
    commands.Pop();
    if (quit) return;
  }
}

bool SimThread::Impl::Publish(Tick tick, std::uint64_t latch_ns,
                              std::uint64_t tick_cost_ns, std::uint64_t hash,
                              bool render) {
  if (config.snapshots == nullptr) {
    pending.Clear();
    return true;
  }
  Snapshot* slot = config.snapshots->AcquireForWrite();
  if (slot == nullptr) {
    // No free slot: skip the render part, keep the reliable deltas.
    ++dropped;
    ++stats.dropped_publishes;
    return false;
  }
  slot->ClearRender();
  SnapshotHeader& header = slot->header;
  header.session = session_id;
  header.generation = generation;
  header.epoch = clock->Epoch();
  header.tick = tick;
  header.latch_ns = latch_ns;
  header.anchor_ns = latch_ns + clock->PeriodNs();
  header.acked_seq = acked_seq;
  header.dropped_publishes = dropped;
  header.tick_cost_ns = tick_cost_ns;
  header.build_cost_ns = 0;
  header.world_hash = hash;
  header.state = state;
  header.render = render && session != nullptr;
  if (header.render) {
    const std::uint64_t build_start = wait_clock->NowNs();
    session->FillRender(*slot);
    header.build_cost_ns = wait_clock->NowNs() - build_start;
  }
  // The reliable section travels with whichever slot publishes first.
  std::swap(slot->reliable, pending);
  pending.Clear();
  config.snapshots->Publish(slot);
  ++stats.publishes;
  Wake();
  return true;
}

bool SimThread::Impl::RunOneTick(Tick tick, std::uint64_t latch_ns,
                                 bool render) {
  std::optional<InputFrame> recorded = session->RecordedFrame(tick);
  InputFrame frame =
      recorded ? *recorded : folder.Latch(tick, latch_ns, *input);
  frame.tick = tick;
  const std::uint64_t start = wait_clock->NowNs();
  std::exception_ptr error;
  TickResult result;
  if (!RunTickContained(*session, frame, latch_ns, result, error)) {
    // Tear the session down here, then post the fault.
    FaultRecord record;
    record.role = ThreadRole::kSim;
    record.breadcrumb = ThreadBreadcrumb().Packed();
    record.tick = tick;
    record.session = session_id;
    DescribeFault(error, record);
    faulted = true;
    EndSession(SessionEventKind::kFaulted);
    fault->Post(error, record);
    Publish(tick, latch_ns, 0, 0, /*render=*/false);
    Wake();
    return false;
  }
  const std::uint64_t cost = wait_clock->NowNs() - start;
  if (const std::uint32_t revision = session->States().Revision();
      revision != stack_revision) {
    // The tick changed the GameStateStack (a scene load, a state switch):
    // its slots are a new generation, never interpolated with older ones.
    stack_revision = revision;
    ++generation;
    Event(SessionEventKind::kGenerationChanged);
  }
  ++stats.ticks;
  ticks_run->fetch_add(1, std::memory_order_release);
  TickRecord record;
  record.session = session_id;
  record.tick = result.tick;
  record.hash = result.hash;
  record.worlds_stepped = result.worlds_stepped;
  pending.ticks.push_back(record);
  Publish(tick, latch_ns, cost, result.hash, render);
  return true;
}

void SimThread::Impl::Run(const std::stop_token& stop) {
  if (config.wait_clock == nullptr) {
    own_clock = MakeSystemWaitClock();
    wait_clock = own_clock.get();
  } else {
    wait_clock = config.wait_clock;
  }
  clock = std::make_unique<PacedClock>(*wait_clock, config.clock);
  while (!stop.stop_requested() && !quit) {
    const std::uint32_t seen = bell.Seen();
    DrainCommands();
    if (quit) break;
    if (!session || state == SimRunState::kSuspended ||
        (state == SimRunState::kPaused && steps_left == 0)) {
      // Idle: the deltas still pending (events, the last ticks' records)
      // go out first; while no slot is free, look again every 1 ms.
      if (!pending.Empty()) {
        const Tick last = session ? session->Driver().NextTick() - 1 : 0;
        if (!Publish(last, wait_clock->NowNs(), 0, 0, /*render=*/false)) {
          wait_clock->SleepUntil(wait_clock->NowNs() + 1'000'000, stop);
          continue;
        }
      }
      bell.Wait(seen, stop);
      continue;
    }
    if (state == SimRunState::kPaused) {
      // Step{n}: at once, without deadlines; each tick publishes.
      const Tick tick = session->Driver().NextTick();
      --steps_left;
      if (!RunOneTick(tick, wait_clock->NowNs(), /*render=*/true)) continue;
      if (steps_left == 0) Event(SessionEventKind::kStepped, 0);
      continue;
    }
    const Tick next = session->Driver().NextTick();
    clock->WaitForDeadline(next, stop);
    if (stop.stop_requested()) break;
    if (!cap_warned && clock->Stats().capped_waits != 0) {
      cap_warned = true;
      SE_LOG_WARN(
          "sim pacing: the timer overshoots by {:.2f} ms (p99), more than the "
          "{:.2f} ms spin cap; ticks may wake late",
          static_cast<double>(clock->OvershootP99Ns()) * 1e-6,
          static_cast<double>(clock->MarginCapNs()) * 1e-6);
    }
    // Commands that arrived during the wait apply at this boundary.
    DrainCommands();
    if (quit || !session || state != SimRunState::kRunning) continue;
    const TickBurst burst = clock->DueTicks(next, wait_clock->NowNs());
    if (burst.count == 0) continue;
    ++stats.bursts;
    if (burst.rebased) {
      ++stats.dilations;
      Event(SessionEventKind::kDilated, burst.dilated_ns);
    }
    for (std::uint32_t i = 0; i < burst.count; ++i) {
      const Tick tick = burst.first + i;
      // Only the burst's last 2 ticks publish their render part, so the
      // platform has a pair to interpolate after a burst (dedicated-
      // driver.md "Data Flow", Snapshot slot).
      const bool render = i + 2 >= burst.count;
      if (!RunOneTick(tick, clock->DeadlineNs(tick), render)) break;
      clock->OnTickRun();
    }
  }
  EndSession(SessionEventKind::kEnded);
  // The ended event, for a platform that still reads.
  Publish(0, wait_clock->NowNs(), 0, 0, /*render=*/false);
}

std::expected<std::unique_ptr<SimThread>, Error> SimThread::Start(
    SimThreadConfig config) {
  auto impl = std::make_unique<Impl>(std::move(config));
  std::unique_ptr<SimThread> sim(new SimThread(std::move(impl)));
  Impl* raw = sim->impl_.get();
  raw->input = &sim->input_;
  raw->fault = &sim->fault_;
  raw->acked = &sim->acked_seq_;
  raw->ticks_run = &sim->ticks_run_;
  auto thread =
      StartThread({.role = ThreadRole::kSim, .name = "sim", .qos = Qos::kHigh},
                  [raw](const std::stop_token& stop) {
                    const SimOwnerScope owner;
                    raw->Run(stop);
                  });
  if (!thread) return std::unexpected(std::move(thread.error()));
  raw->thread = std::move(*thread);
  return sim;
}

SimThread::SimThread(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

SimThread::~SimThread() { Stop(); }

bool SimThread::Send(SimCommand command) {
  command.seq = impl_->next_seq + 1;
  if (!impl_->commands.TryPush(std::move(command))) return false;
  ++impl_->next_seq;
  impl_->bell.Ring();
  return true;
}

void SimThread::Stop() noexcept {
  if (impl_ == nullptr || impl_->stopped) return;
  impl_->stopped = true;
  SimCommand quit;
  quit.op = SimOp::kQuit;
  (void)Send(std::move(quit));
  impl_->thread.RequestStop();
  impl_->bell.Ring();
  impl_->thread.Join();
}

bool SimThread::Running() const noexcept {
  return impl_ != nullptr && impl_->thread.Joinable() && !impl_->stopped;
}

SimThreadStats SimThread::Stats() const noexcept { return impl_->stats; }

}  // namespace se
