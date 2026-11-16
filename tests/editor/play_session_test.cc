// Copyright 2026 The simple-engine-2 Authors.
//
// PlaySession (M10a): Play, Pause, Step and Stop; the document's hash
// across a session; SessionLog edits applied at tick boundaries (at once
// while paused) and discarded at Stop; Play.Faulted from a tick throw and
// from a render-build throw, the World inspectable afterwards; a session's
// per-tick hashes equal a headless run of its start bytes and SessionLog.

#include "se_editor/play_session.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "editor/support/document_world.h"
#include "editor/support/rampage_editor_game.h"
#include "game_rampage/module.h"
#include "game_rampage/view.h"
#include "rampage/support/rampage_session.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/time.h"
#include "se_editor/commands.h"
#include "se_editor/edit_document.h"
#include "se_platform/fs.h"
#include "se_render_core/render_list.h"
#include "se_sim/game_module.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

using testing::kEnemy1;
using testing::kEnemy2;
using testing::kPlayer;

class NoInput final : public InputSource {
 public:
  InputFrame Latch(Tick tick, std::uint64_t /*end*/, bool /*last*/) override {
    InputFrame frame;
    frame.tick = tick;
    return frame;
  }
};

// Everything a session needs, with the sandbox open as the document.
struct Fixture {
  Fixture() SE_REQUIRES(g_main)
      : module(rampage::RampageModule()),
        game(testing::RampageEditorGame()),
        document(testing::OpenSandbox(module.Registry(), game)) {}

  std::unique_ptr<PlaySession> Start(PlayKind kind = PlayKind::kPlay)
      SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner) {
    auto text = document->Serialize();
    REQUIRE(text.has_value());
    PlayStart start;
    start.kind = kind;
    const auto bytes = std::as_bytes(std::span<const char>(*text));
    start.bytes.assign(bytes.begin(), bytes.end());
    start.document_hash = document->Hash();
    start.asset_root = testing::AssetRoot();
    start.seed = 1;
    auto session =
        PlaySession::Start(module, pool, game, std::move(start), nullptr);
    REQUIRE(session.has_value());
    return std::move(*session);
  }

  // |steps| ticks over frames of at most kMaxStepsPerFrame.
  int Ticks(PlaySession& session, int steps) SE_REQUIRES(g_main)
      SE_EXCLUDES(g_sim_owner) {
    int ran = 0;
    do {
      StepPlan plan;
      plan.steps = std::min(steps - ran, kMaxStepsPerFrame);
      const int frame = session.TickFrame(plan, input, audio, 0);
      ran += frame;
      if (frame == 0) break;
    } while (ran < steps);
    return ran;
  }

  ModuleInstance module;
  jobs::ForkJoinPool pool;
  EditorGame game;
  std::unique_ptr<EditDocument> document;
  NoInput input;
  NullAudioSink audio;
};

std::string TransformJson(float x, float y) {
  return std::format(
      R"({{"pos":{{"x":{},"y":{}}},"rot":0,"scale":{{"x":1,"y":1}},)"
      R"("teleport_tick":0}})",
      x, y);
}

TEST_CASE("Play, Pause, Step and Stop", "[play]") {
  const testing::MainThreadScope main_thread;
  Fixture f;
  const std::uint64_t document = f.document->Hash();
  std::unique_ptr<PlaySession> play = f.Start();
  REQUIRE(play->State() == PlayState::kPlaying);
  REQUIRE(play->Running());
  REQUIRE(play->TopWorld() != nullptr);
  REQUIRE(play->TopWorld()->Desc().kind == WorldKind::kPlay);
  REQUIRE(f.Ticks(*play, 3) == 3);
  REQUIRE(play->NextTick() == 4);

  play->Pause();
  REQUIRE(play->State() == PlayState::kPaused);
  REQUIRE_FALSE(play->Running());
  REQUIRE(f.Ticks(*play, 3) == 0);
  // Step: exactly one tick, whatever the plan, then paused again.
  play->Step();
  REQUIRE(play->State() == PlayState::kStepping);
  REQUIRE(f.Ticks(*play, 0) == 1);
  REQUIRE(play->State() == PlayState::kPaused);
  REQUIRE(play->NextTick() == 5);

  play->Resume();
  REQUIRE(f.Ticks(*play, 2) == 2);
  // The markers are in the SessionLog with their ticks.
  std::size_t markers = 0;
  for (const SessionEntry& entry : play->Log().Applied()) {
    if (entry.op == SessionOp::kPause || entry.op == SessionOp::kStep ||
        entry.op == SessionOp::kResume) {
      ++markers;
    }
  }
  REQUIRE(markers == 3);
  play.reset();  // Stop
  REQUIRE(f.document->Hash() == document);
}

TEST_CASE("SessionLog edits apply at tick boundaries and end at Stop",
          "[play]") {
  const testing::MainThreadScope main_thread;
  Fixture f;
  const std::uint64_t document = f.document->Hash();
  std::unique_ptr<PlaySession> play = f.Start();
  REQUIRE(f.Ticks(*play, 2) == 2);
  const World& world = *play->TopWorld();
  // Running: queued for the next tick, not applied before it.
  const std::string moved = TransformJson(64.0F, 64.0F);
  auto set = SetComponentCommand::Make(world, kEnemy1, "Transform", moved);
  REQUIRE(set.has_value());
  REQUIRE(play->Edit(**set, 0).has_value());
  REQUIRE(play->Log().PendingCount() == 1);
  REQUIRE(f.Ticks(*play, 1) == 1);
  REQUIRE(play->Log().PendingCount() == 0);
  REQUIRE(play->Log().Applied().back().apply_before == 3);

  // Paused: applied at once, logged against the next tick.
  play->Pause();
  const std::size_t before = world.EntityCount();
  REQUIRE(play->Edit(DestroyEntityCommand(kEnemy2), 0).has_value());
  REQUIRE(world.EntityCount() == before - 1);
  REQUIRE(play->Log().Applied().back().op == SessionOp::kDestroyEntity);
  REQUIRE(play->Log().Applied().back().apply_before == play->NextTick());

  // Stop: the document never saw them.
  play.reset();
  REQUIRE(f.document->Hash() == document);
  REQUIRE(f.document->Get().Find(kEnemy2).has_value());
}

TEST_CASE("A tick throw is Play.Faulted and the World stays inspectable",
          "[play]") {
  const testing::MainThreadScope main_thread;
  Fixture f;
  std::unique_ptr<PlaySession> play = f.Start();
  REQUIRE(f.Ticks(*play, 5) == 5);
  play->InjectTickThrow();
  static_cast<void>(f.Ticks(*play, 1));
  REQUIRE(play->Faulted());
  REQUIRE(play->FaultInfo().where == "tick");
  REQUIRE(play->FaultInfo().tick == 6);
  REQUIRE_FALSE(play->FaultInfo().what.empty());
  // Frozen: no ticks, no edits; still readable.
  const World* world = play->TopWorld();
  REQUIRE(world != nullptr);
  REQUIRE(world->Find(kPlayer).has_value());
  const std::uint64_t frozen = play->Hash();
  REQUIRE(f.Ticks(*play, 3) == 0);
  REQUIRE(play->Hash() == frozen);
  REQUIRE_FALSE(play->Edit(DestroyEntityCommand(kEnemy1), 0).has_value());
  // The replay holds what ran.
  const std::vector<std::byte> replay = play->FinishReplay();
  auto reader = ReplayReader::Open(replay);
  REQUIRE(reader.has_value());
  REQUIRE(reader->Hashes().size() == 5);
}

TEST_CASE("A render-build throw is Play.Faulted on the guarded path",
          "[play]") {
  const testing::MainThreadScope main_thread;
  Fixture f;
  std::unique_ptr<PlaySession> play = f.Start();
  REQUIRE(f.Ticks(*play, 4) == 4);
  const World& world = *play->TopWorld();
  const Camera2D camera = rampage::SandboxCamera(world);
  RenderList list;
  REQUIRE(play->BuildRenderList(world, camera, 1.0F, list));
  REQUIRE_FALSE(list.quads.empty());
  play->InjectRenderThrow();
  REQUIRE_FALSE(play->BuildRenderList(world, camera, 1.0F, list));
  REQUIRE(play->Faulted());
  REQUIRE(play->FaultInfo().where == "render");
  // The frozen World still draws at alpha 1, and stays readable.
  REQUIRE(play->BuildRenderList(world, camera, 1.0F, list));
  REQUIRE(world.Find(kEnemy1).has_value());
}

TEST_CASE("A Play session replays headless to the same per-tick hashes",
          "[play]") {
  const testing::MainThreadScope main_thread;
  Fixture f;
  std::unique_ptr<PlaySession> play = f.Start();
  REQUIRE(f.Ticks(*play, 30) == 30);
  auto set = SetComponentCommand::Make(*play->TopWorld(), kEnemy1, "Transform",
                                       TransformJson(10.0F, 900.0F));
  REQUIRE(set.has_value());
  REQUIRE(play->Edit(**set, 35).has_value());
  REQUIRE(f.Ticks(*play, 10) == 10);
  play->Pause();
  REQUIRE(play->Edit(DestroyEntityCommand(kEnemy2), 0).has_value());
  play->Step();
  REQUIRE(f.Ticks(*play, 0) == 1);
  play->Resume();
  REQUIRE(f.Ticks(*play, 20) == 20);
  const std::vector<std::byte> bytes = play->FinishReplay();
  auto reader = ReplayReader::Open(bytes);
  REQUIRE(reader.has_value());
  REQUIRE(reader->Hashes().size() == 61);

  // se_replay's path: the start bytes as the scene file, the replay's
  // frames and SessionLog.
  const std::string scene = std::string(SE_TEST_OUT_DIR) + "/replay.scene.json";
  REQUIRE(fs::WriteFile(scene, reader->StartBytes()).has_value());
  testing::RampageSessionOptions options;
  options.scene = testing::SandboxRequest();
  options.scene.path = scene;
  options.seed = reader->Header().seed;
  options.ticks = reader->Frames().size();
  options.input = &*reader;
  const auto session = reader->SessionBytes();
  options.session.assign(session.begin(), session.end());
  const testing::RampageSessionResult result =
      testing::RunRampageSession(options);
  REQUIRE(result.error.empty());
  REQUIRE(result.hashes.size() == reader->Hashes().size());
  for (std::size_t i = 0; i < result.hashes.size(); ++i) {
    INFO("tick " << i + 1);
    REQUIRE(result.hashes[i] == reader->Hashes()[i]);
  }
}

}  // namespace
}  // namespace se::editor
