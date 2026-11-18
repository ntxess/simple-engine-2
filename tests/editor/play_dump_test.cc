// Copyright 2026 The simple-engine-2 Authors.
//
// M12 in the editor (docs/milestone-specs.md "Integration (M10a + M10b +
// M12 + M14)"; ADR-111): what the Editor registers for dumps while Play
// runs, without the Editor: a PlaySession's InputFrames in a dump ring, a
// session header flagged as an editor Play, and the Play-start stream
// holding the session's replay file without frames (PlaySession::DumpBytes),
// attached again as the SessionLog grows. A minidump of those streams
// replays through tools/se_replay's reader to the dump's tick with the
// session's own hashes, the edits made during Play included; a dump from
// Edit mode has nothing to replay. The real dump through crashpad is
// crash.editor_play_hang (label desktop).

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "editor/support/document_world.h"
#include "editor/support/rampage_editor_game.h"
#include "game_rampage/module.h"
#include "se_core/dump_stream.h"
#include "se_core/input_frame.h"
#include "se_core/input_frame_ring.h"
#include "se_core/jobs.h"
#include "se_core/session_header.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/time.h"
#include "se_editor/commands.h"
#include "se_editor/edit_document.h"
#include "se_editor/play_session.h"
#include "se_replay/dump_session.h"
#include "se_runtime/integration.h"
#include "se_sim/game_module.h"
#include "se_sim/replay.h"
#include "se_sim/sim_driver.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

using testing::kEnemy1;
using testing::kEnemy2;

constexpr std::uint32_t kRingStream = 0x53450002;
constexpr std::uint32_t kHeaderStream = 0x53450005;
constexpr std::uint32_t kPlayStartStream = 0x53450006;

class NoInput final : public InputSource {
 public:
  InputFrame Latch(Tick tick, std::uint64_t /*end*/, bool /*last*/) override {
    InputFrame frame;
    frame.tick = tick;
    return frame;
  }
};

std::string TransformJson(float x, float y) {
  return std::format(
      R"({{"pos":{{"x":{},"y":{}}},"rot":0,"scale":{{"x":1,"y":1}},)"
      R"("teleport_tick":0}})",
      x, y);
}

// A Play session with the dump streams the Editor keeps for it.
struct DumpedPlay {
  DumpedPlay() SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner)
      : module(rampage::RampageModule()),
        game(testing::RampageEditorGame()),
        document(testing::OpenSandbox(module.Registry(), game)) {
    auto text = document->Serialize();
    REQUIRE(text.has_value());
    PlayStart start;
    const auto bytes = std::as_bytes(std::span<const char>(*text));
    start.bytes.assign(bytes.begin(), bytes.end());
    start.document_hash = document->Hash();
    start.asset_root = testing::AssetRoot();
    start.seed = 1;
    auto session =
        PlaySession::Start(module, pool, game, std::move(start), nullptr);
    REQUIRE(session.has_value());
    play = std::move(*session);
    // Editor::AttachPlayDump.
    play->AttachInputRing(ring.get());
    const ReplayHeader replay = play->Header();
    SessionHeaderInfo info;
    info.module = "rampage";
    info.scene = "sandbox";
    info.scene_hash = replay.scene_hash;
    info.seed = replay.seed;
    info.tick_rate = replay.tick_rate;
    info.build = replay.build;
    info.platform = replay.platform;
    info.compiler = replay.compiler;
    info.math_hash = replay.math_hash;
    info.cpu_features = replay.cpu_features;
    info.start_tick = play->NextTick();
    info.first_frame = ring->Head();
    info.flags = kSessionFlagPlayStart;
    header->Begin(info);
    Sync();
  }

  // Editor::SyncPlayDump: the stream again when the SessionLog grew.
  void Sync() {
    if (play->LogEntries() == entries && stream.Attached()) return;
    REQUIRE(stream.Attach(play->DumpBytes()).has_value());
    entries = play->LogEntries();
    ++attaches;
  }

  // |steps| ticks, a frame at a time, as Editor::TickFrame runs them.
  void Ticks(int steps) SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner) {
    int ran = 0;
    do {
      StepPlan plan;
      plan.steps = std::min(steps - ran, kMaxStepsPerFrame);
      Sync();
      const int frame = play->TickFrame(plan, input, audio, 0);
      for (const TickResult& result : play->FrameResults()) {
        hashes.push_back(result.hash);
        header->PublishTick(result.tick, result.hash);
      }
      ran += frame;
      if (frame == 0) break;
    } while (ran < steps);
    REQUIRE(ran == std::max(steps, 1));
  }

  [[nodiscard]] std::vector<std::byte> Minidump(bool with_start = true) const {
    std::vector<replay::MinidumpStream> streams = {
        {kRingStream, ring->StreamBytes()},
        {kHeaderStream, header->StreamBytes()},
    };
    if (with_start) streams.push_back({kPlayStartStream, stream.StreamBytes()});
    return replay::WriteMinidump(streams);
  }

  ModuleInstance module;
  jobs::ForkJoinPool pool;
  EditorGame game;
  std::unique_ptr<EditDocument> document;
  NoInput input;
  NullAudioSink audio;
  // Dump streams are never destroyed while registered; these are not
  // registered with a handler, but they are as large as the real ones.
  std::unique_ptr<InputFrameRing> ring = std::make_unique<InputFrameRing>();
  std::unique_ptr<SessionHeaderStream> header =
      std::make_unique<SessionHeaderStream>();
  integration::PlayStartStream stream;
  std::unique_ptr<PlaySession> play;
  std::size_t entries = 0;
  int attaches = 0;
  std::vector<std::uint64_t> hashes;
};

std::string StartScene(const char* name) {
  return std::string(SE_TEST_OUT_DIR) + "/" + name;
}

TEST_CASE("A dump taken during Play replays to its tick, edits included",
          "[play][dump]") {
  const testing::MainThreadScope main_thread;
  DumpedPlay f;
  REQUIRE(f.attaches == 1);
  f.Ticks(30);
  // An edit against a later tick, then one while paused, a step, a resume.
  auto set = SetComponentCommand::Make(
      *f.play->TopWorld(), kEnemy1, "Transform", TransformJson(10.0F, 900.0F));
  REQUIRE(set.has_value());
  REQUIRE(f.play->Edit(**set, 35).has_value());
  f.Ticks(10);
  f.play->Pause();
  REQUIRE(f.play->Edit(DestroyEntityCommand(kEnemy2), 0).has_value());
  f.play->Step();
  f.Ticks(0);
  f.play->Resume();
  f.Ticks(20);
  REQUIRE(f.hashes.size() == 61);
  // The stream followed the log: the edit, the pause and its edit and the
  // step (one sync before the stepped tick), the resume.
  REQUIRE(f.attaches == 4);

  const std::vector<std::byte> dump = f.Minidump();
  auto session = replay::ReadDumpedSession(dump);
  REQUIRE(session.has_value());
  REQUIRE((session->header.flags & kSessionFlagPlayStart) != 0);
  REQUIRE(session->frames.size() == 61);
  REQUIRE_FALSE(session->play_start.empty());
  // The stream is the session's replay file without frames.
  auto file = ReplayReader::Open(session->play_start);
  REQUIRE(file.has_value());
  CHECK(file->Frames().empty());
  CHECK(file->StartBytes().size() == f.play->StartBytes().size());
  CHECK_FALSE(file->SessionBytes().empty());

  const replay::FromDumpResult result = replay::ReplayDumpedRampage(
      *session, SE_SOURCE_DIR, StartScene("dump-play-start.scene.json"));
  INFO(result.error);
  REQUIRE(result.error.empty());
  // No breadcrumb in this dump: the replay runs every frame of the ring and
  // checks the last published hash.
  REQUIRE(result.last_tick == 61);
  REQUIRE(result.checked_tick == 61);
  REQUIRE(result.hashes.size() == f.hashes.size());
  for (std::size_t i = 0; i < result.hashes.size(); ++i) {
    INFO("tick " << i + 1);
    REQUIRE(result.hashes[i] == f.hashes[i]);
  }

  // The edits matter: without the stream's SessionLog the replay arrives
  // somewhere else, and the published hash catches it.
  replay::DumpedSession no_log = *session;
  ReplayRecorder bare;
  bare.Begin(f.play->Header(), f.play->StartBytes());
  no_log.play_start = bare.Finish();
  const replay::FromDumpResult diverged = replay::ReplayDumpedRampage(
      no_log, SE_SOURCE_DIR, StartScene("dump-no-log.scene.json"));
  CHECK(diverged.error.find("tick 61 replays to hash") != std::string::npos);
}

TEST_CASE("A Play dump without its start, or from Edit mode, says so",
          "[play][dump]") {
  const testing::MainThreadScope main_thread;
  DumpedPlay f;
  f.Ticks(5);
  // The Play-start stream withdrawn (Stop came before the dump's read):
  // nothing to start from.
  auto session = replay::ReadDumpedSession(f.Minidump(/*with_start=*/false));
  REQUIRE(session.has_value());
  CHECK(session->play_start.empty());
  replay::FromDumpResult result = replay::ReplayDumpedRampage(
      *session, SE_SOURCE_DIR, StartScene("dump-missing.scene.json"));
  CHECK(result.error.find("no Play-start stream") != std::string::npos);
  CHECK(result.hashes.empty());

  // An editor Play needs a file for its start bytes.
  session = replay::ReadDumpedSession(f.Minidump());
  REQUIRE(session.has_value());
  result = replay::ReplayDumpedRampage(*session, SE_SOURCE_DIR);
  CHECK(result.error.find("--start-scene") != std::string::npos);

  // Stop: the stream goes and the header says that no session runs
  // (Editor::DetachPlayDump).
  f.stream.Detach();
  CHECK(f.stream.StreamBytes().empty());
  SessionHeaderInfo idle;
  idle.module = "rampage";
  idle.scene = "sandbox";
  idle.start_tick = 0;
  idle.first_frame = f.ring->Head();
  idle.flags = kSessionFlagNoSession;
  f.header->Begin(idle);
  session = replay::ReadDumpedSession(f.Minidump(/*with_start=*/false));
  REQUIRE(session.has_value());
  CHECK(session->frames.empty());
  result = replay::ReplayDumpedRampage(*session, SE_SOURCE_DIR,
                                       StartScene("dump-idle.scene.json"));
  CHECK(result.error.find("no running session") != std::string::npos);
}

}  // namespace
}  // namespace se::editor
