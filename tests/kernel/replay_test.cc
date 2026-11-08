// Copyright 2026 The simple-engine-2 Authors.
//
// Replays: the BEVE file is real BEVE (glaze converts it to JSON), frames,
// hashes and the SessionLog round-trip, and a recorded kernel session
// replays to the same per-tick hashes at another frame rate.

#include "se_sim/replay.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <glaze/beve/beve_to_json.hpp>
#include <string>
#include <vector>

#include "se_core/detmath.h"
#include "se_core/input_frame.h"
#include "se_sim/beve.h"
#include "se_sim/session_log.h"
#include "support/kernel_session.h"
#include "support/kernel_test_module.h"
#include "support/test_roles.h"

namespace se {
namespace {

InputFrame SampleFrame(Tick tick) {
  InputFrame frame;
  frame.tick = tick;
  frame.held = 0x8000000000000005ULL;
  frame.pressed[3] = 2;
  frame.released[63] = 1;
  frame.axes[7] = -0.5F;
  frame.platform_live = true;
  frame.edge_count = 2;
  frame.edges[0] =
      InputEdge{3, true, 123456789ULL, 0, EdgeSource::kRawKeyboard};
  frame.edges[1] = InputEdge{3, false, 123456999ULL, 1, EdgeSource::kMouse};
  return frame;
}

TEST_CASE("A replay round-trips header, frames, hashes and session",
          "[replay]") {
  const std::vector<std::byte> scene = {std::byte{1}, std::byte{2}};
  const ReplayHeader header = MakeReplayHeader(scene, 99, 120);
  CHECK(header.math_hash == dm::SelfTestHash());
  CHECK_FALSE(header.platform.empty());
  CHECK_FALSE(header.compiler.empty());

  ReplayRecorder recorder;
  CHECK_FALSE(recorder.Active());
  recorder.Begin(header, scene);
  for (Tick t = 5; t < 9; ++t) {
    recorder.Record(SampleFrame(t));
    recorder.RecordHash(t, t * 1000 + 7);
  }
  SessionLog log;
  log.AddScheduled(MakeMarker(6, SessionOp::kPause));
  recorder.RecordSession(log);
  const std::vector<std::byte> bytes = recorder.Finish();
  CHECK_FALSE(recorder.Active());

  auto reader = ReplayReader::Open(bytes);
  REQUIRE(reader.has_value());
  CHECK(reader->Header() == header);
  CHECK(reader->MathMatches());
  REQUIRE(reader->StartBytes().size() == 2);
  REQUIRE(reader->FirstTick() == 5);
  REQUIRE(reader->Frames().size() == 4);
  CHECK(reader->Frames()[2] == SampleFrame(7));
  CHECK(reader->Latch(8, 0, true) == SampleFrame(8));
  const InputFrame past = reader->Latch(20, 0, true);
  REQUIRE(past.tick == 20);
  REQUIRE(past.held == 0);
  REQUIRE(reader->FirstHashTick() == 5);
  REQUIRE(reader->Hashes().size() == 4);
  REQUIRE(reader->Hashes()[3] == 8007);
  const auto session = SessionLog::Read(reader->SessionBytes());
  REQUIRE(session.has_value());
  REQUIRE(session->PendingCount() == 1);
}

TEST_CASE("Replay files are BEVE that glaze reads", "[replay]") {
  ReplayRecorder recorder;
  recorder.Begin(MakeReplayHeader({}, 1, 120), {});
  recorder.Record(SampleFrame(1));
  recorder.RecordHash(1, 0xFFFFFFFFFFFFFFFFULL);
  const std::vector<std::byte> bytes = recorder.Finish();
  std::string json;
  const auto ec = glz::beve_to_json(bytes, json);
  INFO(json);
  REQUIRE_FALSE(ec);
  CHECK(json.find(R"("format":"se-replay")") != std::string::npos);
  CHECK(json.find(R"("tick_rate":120)") != std::string::npos);
  CHECK(json.find(R"("hashes":[18446744073709551615])") != std::string::npos);
  CHECK(json.find(R"("first_tick":1)") != std::string::npos);

  // The first-party parser reads its own output, and nothing else.
  CHECK(beve::Parse(bytes).has_value());
  std::vector<std::byte> truncated = bytes;
  truncated.pop_back();
  CHECK_FALSE(ReplayReader::Open(truncated).has_value());
  CHECK_FALSE(ReplayReader::Open({}).has_value());
}

TEST_CASE("A recorded session replays to the same hashes at another rate",
          "[replay][determinism]") {
  testing::MainThreadScope main_thread;
  testing::KernelSessionOptions record;
  record.hz = 144.0;
  record.jitter_sec = 1.0e-3;
  record.ticks = 420;
  record.edits_path = testing::EditsFixturePath();
  record.record = true;
  const testing::KernelSessionResult recorded =
      testing::RunKernelSession(record);
  REQUIRE(recorded.error.empty());
  REQUIRE(recorded.hashes.size() == 420);
  REQUIRE_FALSE(recorded.replay.empty());

  auto reader = ReplayReader::Open(recorded.replay);
  REQUIRE(reader.has_value());
  REQUIRE(reader->Frames().size() == 420);
  CHECK(std::vector<std::uint64_t>(reader->Hashes().begin(),
                                   reader->Hashes().end()) == recorded.hashes);

  testing::KernelSessionOptions replay;
  replay.hz = 60.0;
  replay.ticks = 420;
  replay.replay = &*reader;
  const testing::KernelSessionResult replayed =
      testing::RunKernelSession(replay);
  REQUIRE(replayed.error.empty());
  CHECK(replayed.hashes == recorded.hashes);
  CHECK(replayed.session_applied == recorded.session_applied);
  REQUIRE(replayed.session_failures == 0);
}

}  // namespace
}  // namespace se
