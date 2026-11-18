// Copyright 2026 The simple-engine-2 Authors.
//
// The dedicated sim thread (docs/milestone-specs.md M17a; ADR-083),
// headless, label unit (TSan in linux-clang-tsan):
//   - inline against threaded: identical per-tick hashes at W = 0 on the
//     kernel golden (determinism_kernel, its session log included) and on
//     the sandbox golden (determinism_rampage) and M3's three reference
//     scenarios (effects, patrol, stress) over 3600 ticks; the threaded
//     driver plays recorded InputFrames (live latching differs by design);
//   - commands: Pause, Step{n} runs exactly n ticks, Resume, Quit joins;
//   - a throw out of a tick is contained: the session ends on the sim
//     thread and the FaultBox holds the fault;
//   - a catch-up burst renders its last 2 ticks;
//   - the snapshots the sim thread fills (AppendRenderView) give, through
//     BuildSnapshotRenderList, the RenderLists the inline driver builds from
//     its World (BuildRenderList) on rampage's scenarios, tick by tick, at
//     alpha 0.5, 0.75 and 1.
// The same comparisons run at W = 1 and 3 (M14): both drivers build their
// session's ForkJoinPool from one configuration (every site enabled, a
// small grain so the scenes fork), the threaded driver's on the sim thread.
//
// The sim thread paces itself on a ManualClock that advances on every wait,
// so runs take as long as their ticks.

#include "se_sim/sim_thread.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "game_rampage/module.h"
#include "game_rampage/view.h"
#include "rampage/support/rampage_session.h"
#include "rampage/support/seinput.h"
#include "se_core/error.h"
#include "se_core/hash.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/snapshot.h"
#include "se_core/snapshot_channel.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_render_core/render_list.h"
#include "se_render_core/snapshot_fill.h"
#include "se_render_core/snapshot_render_source.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/paced_clock.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"
#include "support/fork_config.h"
#include "support/golden.h"
#include "support/kernel_session.h"
#include "support/kernel_test_module.h"
#include "support/test_roles.h"

namespace se {
namespace {

// A game module run by the sim thread from recorded frames: built and
// destroyed on the sim thread.
class RecordedSession final : public SimSession {
 public:
  // With |camera| set, each published slot carries the top state's Worlds
  // drawn through it (FillRender).
  using CameraFn = std::function<Camera2D(const World&)>;

  RecordedSession(const GameModule& module, std::uint64_t seed,
                  std::vector<InputFrame> frames, SessionLog log,
                  CameraFn camera = {}, jobs::ForkJoinPool::Config pool = {})
      SE_REQUIRES(g_sim_owner)
      : module_(module),
        states_(module_.Factory(), Env(module_, seed)),
        pool_(std::move(pool)),
        driver_(module_.MutableSchedule(), pool_),
        log_(std::move(log)),
        frames_(std::move(frames)),
        camera_(std::move(camera)) {
    states_.Request(StackOp::kPush, module_.Module().start_state);
    states_.ApplyPending();
  }

  GameStateStack& States() override { return states_; }
  SimDriver& Driver() override { return driver_; }
  SessionLog& Log() override { return log_; }
  ReplayRecorder& Recorder() override { return recorder_; }
  AudioSink& Audio() override { return audio_; }
  std::optional<InputFrame> RecordedFrame(Tick tick) override {
    InputFrame frame;
    if (tick >= 1 && tick <= frames_.size()) {
      frame = frames_[static_cast<std::size_t>(tick - 1)];
    }
    frame.tick = tick;
    return frame;
  }
  void FillRender(Snapshot& slot) override {
    if (!camera_) return;
    for (const World* world : states_.TopWorlds()) {
      RenderViewOptions options;
      options.camera = camera_(*world);
      options.stepped = states_.SimRunning();
      AppendRenderView(*world, options, slot);
    }
  }

 private:
  static StateEnv Env(const ModuleInstance& module, std::uint64_t seed) {
    StateEnv env;
    env.registry = &module.Registry();
    env.seed = seed;
    return env;
  }

  ModuleInstance module_;
  GameStateStack states_;
  jobs::ForkJoinPool pool_;
  SimDriver driver_;
  SessionLog log_;
  ReplayRecorder recorder_;
  NullAudioSink audio_;
  std::vector<InputFrame> frames_;
  CameraFn camera_;
};

struct ThreadedRun {
  std::vector<std::uint64_t> hashes;
  std::vector<SessionEvent> events;
  std::uint32_t slots = 0;
  bool faulted = false;
  std::string fault;
  SimThreadStats stats;
};

// Receives every slot until |ticks| tick records arrived (or the sim
// faulted), then stops the thread and drains the rest.
ThreadedRun RunThreaded(SimSessionFactory factory, std::size_t ticks) {
  ManualClock clock;
  SnapshotChannel channel(8);
  SimThreadConfig config;
  config.factory = std::move(factory);
  config.wait_clock = &clock;
  config.snapshots = &channel;
  auto started = SimThread::Start(std::move(config));
  REQUIRE(started.has_value());
  std::unique_ptr<SimThread> sim = std::move(*started);
  ThreadedRun run;
  const auto receive = [&run, &channel] {
    channel.Receive([&run](SlotLease lease) {
      ++run.slots;
      for (const TickRecord& record : lease->reliable.ticks) {
        run.hashes.push_back(record.hash);
      }
      for (const SessionEvent& event : lease->reliable.events) {
        run.events.push_back(event);
      }
    });
  };
  REQUIRE(sim->Send({.op = SimOp::kStartGame}));
  // A bound on the spins instead of a wall clock (a deterministic module).
  for (std::uint64_t spin = 0; spin < 2'000'000'000ULL; ++spin) {
    receive();
    if (run.hashes.size() >= ticks || sim->Fault().Posted()) break;
    std::this_thread::yield();
  }
  sim->Stop();
  receive();
  run.stats = sim->Stats();
  run.faulted = sim->Fault().Posted();
  if (run.faulted) run.fault = std::string(sim->Fault().Record().Message());
  return run;
}

std::vector<std::uint64_t> FirstN(const std::vector<std::uint64_t>& hashes,
                                  std::size_t n) {
  REQUIRE(hashes.size() >= n);
  return {hashes.begin(), hashes.begin() + static_cast<std::ptrdiff_t>(n)};
}

// The pool both drivers of a comparison build: W workers, every site
// enabled, grain 2 (W = 0: the shipped inline pool).
jobs::ForkJoinPool::Config PoolFor(int workers) {
  if (workers == 0) return {};
  testing::ForkTestOptions options;
  options.grain = 2;
  return testing::ForkTestConfig(workers, options);
}

TEST_CASE("Threaded hashes equal inline ones on the kernel golden",
          "[sim_thread][golden]") {
  const testing::MainThreadScope main;
  for (const int workers : {0, 1, 3}) {
    INFO("W = " << workers);
    // determinism_test.cc's options: one tick per frame, the logged edits.
    testing::KernelSessionOptions options;
    options.clock_seed = 11;
    options.edits_path = testing::EditsFixturePath();
    options.record = true;
    options.pool = PoolFor(workers);
    const testing::KernelSessionResult inline_run =
        testing::RunKernelSession(options);
    REQUIRE(inline_run.error.empty());
    REQUIRE(inline_run.hashes.size() == 600);
    auto reader = ReplayReader::Open(inline_run.replay);
    REQUIRE(reader.has_value());
    const std::vector<InputFrame> frames(reader->Frames().begin(),
                                         reader->Frames().end());
    const std::vector<std::byte> session(reader->SessionBytes().begin(),
                                         reader->SessionBytes().end());
    const ThreadedRun threaded = RunThreaded(
        [&frames, &session,
         workers]() -> std::expected<std::unique_ptr<SimSession>, Error> {
          AssertSimThread();
          auto log = SessionLog::Read(session);
          if (!log) return std::unexpected(std::move(log.error()));
          return std::make_unique<RecordedSession>(
              testing::KernelTestModule(), 42, frames, std::move(*log),
              RecordedSession::CameraFn{}, PoolFor(workers));
        },
        600);
    REQUIRE_FALSE(threaded.faulted);
    CHECK(FirstN(threaded.hashes, 600) == inline_run.hashes);
    const testing::GoldenResult golden = testing::CheckGolden(
        "determinism_kernel", FirstN(threaded.hashes, 600));
    INFO(golden.message);
    CHECK(golden.ok);
  }
}

struct Scenario {
  const char* name;
  bool sandbox;
};

TEST_CASE("Threaded hashes equal inline ones on rampage's scenarios",
          "[sim_thread][rampage]") {
  const testing::MainThreadScope main;
  for (const int workers : {0, 1, 3}) {
    for (const Scenario scenario :
         {Scenario{"sandbox", true}, Scenario{"effects", false},
          Scenario{"patrol", false}, Scenario{"stress", false}}) {
      INFO("W = " << workers << ", scenario " << scenario.name);
      testing::RampageSessionOptions options;
      options.ticks = 3600;
      options.pool = PoolFor(workers);
      std::vector<InputFrame> frames;
      std::optional<testing::FrameListInput> recorded;
      if (scenario.sandbox) {
        options.scene = testing::SandboxSceneRequest();
        for (Tick tick = 1; tick <= options.ticks; ++tick) {
          frames.push_back(
              testing::RampageScriptedInput::FrameFor(tick, options.seed));
        }
      } else {
        options.scene = testing::ReferenceScenarioRequest(scenario.name);
        auto read =
            testing::ReadSeinput(testing::RampageFixtureDir() + "/reference/" +
                                 scenario.name + ".seinput");
        REQUIRE(read.has_value());
        frames = *read;
        recorded.emplace(std::move(*read));
        options.input = &*recorded;
      }
      const testing::RampageSessionResult inline_run =
          testing::RunRampageSession(options);
      REQUIRE(inline_run.error.empty());
      REQUIRE(inline_run.hashes.size() == 3600);
      // The sandbox state reads its scene request when it enters.
      rampage::SetSandboxScene(options.scene);
      const std::uint64_t seed = options.seed;
      const ThreadedRun threaded = RunThreaded(
          [&frames, seed,
           workers]() -> std::expected<std::unique_ptr<SimSession>, Error> {
            AssertSimThread();
            return std::make_unique<RecordedSession>(
                rampage::RampageModule(), seed, frames, SessionLog{},
                RecordedSession::CameraFn{}, PoolFor(workers));
          },
          3600);
      REQUIRE_FALSE(threaded.faulted);
      CHECK(FirstN(threaded.hashes, 3600) == inline_run.hashes);
      if (scenario.sandbox) {
        const testing::GoldenResult golden = testing::CheckGolden(
            "determinism_rampage", FirstN(threaded.hashes, 3600));
        INFO(golden.message);
        CHECK(golden.ok);
      }
    }
  }
}

// The kernel module from its scripted input, for the command tests.
SimSessionFactory KernelFactory() {
  return []() -> std::expected<std::unique_ptr<SimSession>, Error> {
    AssertSimThread();
    std::vector<InputFrame> frames;
    return std::make_unique<RecordedSession>(testing::KernelTestModule(), 42,
                                             std::move(frames), SessionLog{});
  };
}

TEST_CASE("Pause, Step{n}, Resume and Quit", "[sim_thread]") {
  ManualClock clock;
  SnapshotChannel channel(8);
  SimThreadConfig config;
  config.factory = KernelFactory();
  config.wait_clock = &clock;
  config.snapshots = &channel;
  auto started = SimThread::Start(std::move(config));
  REQUIRE(started.has_value());
  std::unique_ptr<SimThread> sim = std::move(*started);
  std::vector<Tick> ticks;
  std::vector<SessionEventKind> events;
  std::uint32_t acked = 0;
  const auto receive = [&] {
    channel.Receive([&](SlotLease lease) {
      for (const TickRecord& record : lease->reliable.ticks) {
        ticks.push_back(record.tick);
      }
      for (const SessionEvent& event : lease->reliable.events) {
        events.push_back(event.kind);
      }
      acked = lease->header.acked_seq;
    });
  };
  const auto until = [&](auto done) {
    for (std::uint64_t spin = 0; spin < 2'000'000'000ULL && !done(); ++spin) {
      receive();
      std::this_thread::yield();
    }
    return done();
  };
  REQUIRE(sim->Send({.op = SimOp::kStartGame}));
  REQUIRE(until([&] { return ticks.size() >= 50; }));
  REQUIRE(sim->Send({.op = SimOp::kPause}));
  REQUIRE(until([&] { return sim->AckedSeq() >= 2; }));
  // Paused: the ticks that ran before the pause all arrive (the idle sim
  // publishes its pending records), and no tick runs after it.
  const std::uint64_t run_at_pause = sim->TicksRun();
  REQUIRE(until([&] { return ticks.size() >= run_at_pause; }));
  for (int i = 0; i < 2000; ++i) {
    receive();
    std::this_thread::yield();
  }
  CHECK(sim->TicksRun() == run_at_pause);
  CHECK(ticks.size() == run_at_pause);
  // Step{5}: exactly five ticks, then paused again.
  REQUIRE(sim->Send({.op = SimOp::kStep, .ticks = 5}));
  REQUIRE(until([&] { return ticks.size() >= run_at_pause + 5; }));
  for (int i = 0; i < 2000; ++i) {
    receive();
    std::this_thread::yield();
  }
  CHECK(sim->TicksRun() == run_at_pause + 5);
  CHECK(ticks.size() == run_at_pause + 5);
  for (std::size_t i = 1; i < ticks.size(); ++i) {
    REQUIRE(ticks[i] == ticks[i - 1] + 1);
  }
  // Resume: ticks run again.
  REQUIRE(sim->Send({.op = SimOp::kResume}));
  REQUIRE(until([&] { return ticks.size() >= run_at_pause + 5 + 20; }));
  // Quit joins.
  sim->Stop();
  CHECK_FALSE(sim->Running());
  receive();
  REQUIRE(acked >= 3);
  const auto has = [&events](SessionEventKind kind) {
    for (const SessionEventKind seen : events) {
      if (seen == kind) return true;
    }
    return false;
  };
  CHECK(has(SessionEventKind::kStarted));
  CHECK(has(SessionEventKind::kPaused));
  CHECK(has(SessionEventKind::kStepped));
  CHECK(has(SessionEventKind::kResumed));
}

TEST_CASE("A catch-up burst publishes render state for its last 2 ticks",
          "[sim_thread]") {
  // The time moves only when the test says so.
  ManualClock clock(ManualClock::Mode::kBlocking);
  SnapshotChannel channel(8);
  SimThreadConfig config;
  config.factory = KernelFactory();
  config.wait_clock = &clock;
  config.snapshots = &channel;
  auto started = SimThread::Start(std::move(config));
  REQUIRE(started.has_value());
  std::unique_ptr<SimThread> sim = std::move(*started);
  std::vector<Tick> ticks;
  std::vector<Tick> rendered;
  std::uint32_t dropped = 0;
  const auto receive = [&] {
    channel.Receive([&](SlotLease lease) {
      for (const TickRecord& record : lease->reliable.ticks) {
        ticks.push_back(record.tick);
      }
      if (lease->header.render) rendered.push_back(lease->header.tick);
      dropped = lease->header.dropped_publishes;
    });
  };
  const auto until = [&](auto done) {
    for (std::uint64_t spin = 0; spin < 2'000'000'000ULL && !done(); ++spin) {
      receive();
      std::this_thread::yield();
    }
    return done();
  };
  REQUIRE(sim->Send({.op = SimOp::kStartGame}));
  // Tick 1 is due when the session starts.
  REQUIRE(until([&] { return !ticks.empty(); }));
  // Six deadlines pass at once (6 x 1/120 s): ticks 2..7 run as one burst.
  clock.Advance(50'000'000);
  REQUIRE(until([&] { return ticks.size() >= 7; }));
  sim->Stop();
  receive();
  REQUIRE(dropped == 0);
  REQUIRE(sim->Stats().bursts == 2);
  REQUIRE(ticks.size() == 7);
  // A burst of one renders its tick; a burst of six its last two.
  CHECK(rendered == std::vector<Tick>{1, 6, 7});
}

// A RenderList's quads (bits), batches and stats but the interpolated
// count.
std::uint64_t ListHash(const RenderList& list) {
  Hasher hasher;
  hasher.AddBytes(std::as_bytes(std::span(list.quads)));
  for (const QuadBatch& batch : list.batches) {
    hasher.AddU64(batch.texture);
    hasher.AddU64(batch.first);
    hasher.AddU64(batch.count);
  }
  hasher.AddU64(list.stats.sprites);
  hasher.AddU64(list.stats.culled);
  return hasher.Finish();
}

constexpr std::array<float, 3> kListAlphas{0.5F, 0.75F, 1.0F};

TEST_CASE("Threaded snapshots draw the inline driver's RenderLists",
          "[sim_thread][rampage][snapshot]") {
  const testing::MainThreadScope main;
  for (const Scenario scenario :
       {Scenario{"sandbox", true}, Scenario{"effects", false},
        Scenario{"patrol", false}, Scenario{"stress", false}}) {
    INFO("scenario " << scenario.name);
    testing::RampageSessionOptions options;
    options.ticks = 3600;
    std::vector<InputFrame> frames;
    std::optional<testing::FrameListInput> recorded;
    if (scenario.sandbox) {
      options.scene = testing::SandboxSceneRequest();
      for (Tick tick = 1; tick <= options.ticks; ++tick) {
        frames.push_back(
            testing::RampageScriptedInput::FrameFor(tick, options.seed));
      }
    } else {
      options.scene = testing::ReferenceScenarioRequest(scenario.name);
      auto read =
          testing::ReadSeinput(testing::RampageFixtureDir() + "/reference/" +
                               scenario.name + ".seinput");
      REQUIRE(read.has_value());
      frames = *read;
      recorded.emplace(std::move(*read));
      options.input = &*recorded;
    }
    // Inline: the World after each tick, drawn at each alpha.
    std::vector<std::uint64_t> inline_lists;
    std::vector<std::uint32_t> inline_interpolated;
    std::uint32_t sprites = 0;
    RenderList list;
    options.on_world = [&](const World& world) {
      if (world.Clock().tick == 0) return;
      for (const float alpha : kListAlphas) {
        BuildRenderList(world, rampage::SandboxCamera(world), alpha, list);
        inline_lists.push_back(ListHash(list));
        inline_interpolated.push_back(list.stats.interpolated);
      }
      sprites = std::max(sprites, list.stats.sprites);
    };
    const testing::RampageSessionResult inline_run =
        testing::RunRampageSession(options);
    REQUIRE(inline_run.error.empty());
    REQUIRE(inline_lists.size() == kListAlphas.size() * options.ticks);
    REQUIRE(sprites > 0);
    // The pairs interpolate: most sprites move between ticks.
    std::uint64_t interpolated = 0;
    for (const std::uint32_t count : inline_interpolated) interpolated += count;
    REQUIRE(interpolated > options.ticks);

    // Threaded, one tick per deadline: the test moves a blocking clock to
    // each deadline once the previous tick's slot has arrived, so every
    // tick publishes its render part.
    rampage::SetSandboxScene(options.scene);
    const std::uint64_t seed = options.seed;
    ManualClock clock(ManualClock::Mode::kBlocking);
    const std::uint64_t t0 = clock.NowNs();
    SnapshotChannel channel(8);
    SimThreadConfig config;
    config.factory =
        [&frames, seed]() -> std::expected<std::unique_ptr<SimSession>, Error> {
      AssertSimThread();
      return std::make_unique<RecordedSession>(
          rampage::RampageModule(), seed, frames, SessionLog{},
          [](const World& world) { return rampage::SandboxCamera(world); });
    };
    config.wait_clock = &clock;
    config.snapshots = &channel;
    auto started = SimThread::Start(std::move(config));
    REQUIRE(started.has_value());
    std::unique_ptr<SimThread> sim = std::move(*started);
    REQUIRE(sim->Send({.op = SimOp::kStartGame}));
    SlotLease previous;
    Tick seen = 0;
    std::uint64_t compared = 0;
    std::uint64_t mismatches = 0;
    std::string first_mismatch;
    for (Tick k = 1; k <= options.ticks; ++k) {
      clock.SetNow(t0 + (k - 1) * 1'000'000'000ULL / kDefaultTickRate);
      for (std::uint64_t spin = 0; spin < 2'000'000'000ULL; ++spin) {
        channel.Receive([&](SlotLease lease) {
          if (!lease->header.render) return;
          const Tick tick = lease->header.tick;
          const Snapshot* older = previous ? previous.Get() : nullptr;
          for (std::size_t i = 0; i < kListAlphas.size(); ++i) {
            // The first tick has no pair: only alpha 1 is the inline list.
            if (older == nullptr && kListAlphas[i] < 1.0F) continue;
            BuildSnapshotRenderList(older, *lease, 0, kListAlphas[i], list);
            ++compared;
            const std::size_t at = (tick - 1) * kListAlphas.size() + i;
            // Without S_a nothing is interpolated; the inline list of tick
            // 1 interpolates from the loaded state (the same quads at
            // alpha 1).
            const bool same_count =
                older == nullptr ||
                list.stats.interpolated == inline_interpolated[at];
            if (ListHash(list) != inline_lists[at] || !same_count) {
              if (mismatches++ == 0) {
                first_mismatch =
                    std::format("tick {} alpha {}", tick, kListAlphas[i]);
              }
            }
          }
          seen = tick;
          previous = std::move(lease);
        });
        if (seen >= k || sim->Fault().Posted()) break;
        std::this_thread::yield();
      }
      REQUIRE_FALSE(sim->Fault().Posted());
      REQUIRE(seen == k);
    }
    previous.Reset();
    sim->Stop();
    channel.Receive([](SlotLease /*lease*/) {});
    INFO("first mismatch: " << first_mismatch << " (" << mismatches
                            << " in all)");
    REQUIRE(mismatches == 0);
    CHECK(compared == kListAlphas.size() * options.ticks - 2);
    REQUIRE(sim->Stats().dropped_publishes == 0);
  }
}

// A session whose tick throws once it reaches tick 30.
class ThrowingAudio final : public AudioSink {
 public:
  void Drain(World& world, std::uint64_t /*tick_window_end_ns*/) override {
    if (world.Clock().tick >= 30) throw std::runtime_error("tick 30 throws");
  }
};

class ThrowingSession final : public SimSession {
 public:
  ThrowingSession() SE_REQUIRES(g_sim_owner)
      : module_(testing::KernelTestModule()),
        states_(module_.Factory(), Env()),
        driver_(module_.MutableSchedule(), pool_) {
    states_.Request(StackOp::kPush, module_.Module().start_state);
    states_.ApplyPending();
  }
  GameStateStack& States() override { return states_; }
  SimDriver& Driver() override { return driver_; }
  SessionLog& Log() override { return log_; }
  ReplayRecorder& Recorder() override { return recorder_; }
  AudioSink& Audio() override { return audio_; }
  std::optional<InputFrame> RecordedFrame(Tick tick) override {
    InputFrame frame;
    frame.tick = tick;
    return frame;
  }

 private:
  StateEnv Env() const {
    StateEnv env;
    env.registry = &module_.Registry();
    env.seed = 42;
    return env;
  }

  ModuleInstance module_;
  GameStateStack states_;
  jobs::ForkJoinPool pool_;
  SimDriver driver_;
  SessionLog log_;
  ReplayRecorder recorder_;
  ThrowingAudio audio_;
};

TEST_CASE("A throw out of a tick ends the session and posts the fault",
          "[sim_thread]") {
  const ThreadedRun run = RunThreaded(
      []() -> std::expected<std::unique_ptr<SimSession>, Error> {
        AssertSimThread();
        return std::make_unique<ThrowingSession>();
      },
      1000);
  REQUIRE(run.faulted);
  REQUIRE(run.fault == "tick 30 throws");
  // Ticks 1-29 ran; tick 30 did not complete.
  REQUIRE(run.hashes.size() == 29);
  bool ended = false;
  for (const SessionEvent& event : run.events) {
    if (event.kind == SessionEventKind::kFaulted) ended = true;
  }
  CHECK(ended);
}

}  // namespace
}  // namespace se
