// Copyright 2026 The simple-engine-2 Authors.
//
// Replays: the BEVE file is real BEVE (glaze converts it to JSON), frames,
// hashes and the SessionLog round-trip, corrupt files are errors, and a
// recorded kernel session replays to the same per-tick hashes at another
// frame rate.

#include "se_sim/replay.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <glaze/beve/beve_to_json.hpp>
#include <span>
#include <string>
#include <string_view>
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
  frame.edge_count = 4;
  frame.edges[0] =
      InputEdge{3, true, 123456789ULL, 0, EdgeSource::kRawKeyboard};
  frame.edges[1] = InputEdge{3, false, 123456999ULL, 1, EdgeSource::kMouse};
  // Every source survives a replay (M8), the cleared releases included.
  frame.edges[2] =
      InputEdge{5, true, 123457000ULL, 0, EdgeSource::kGamepadSampler};
  frame.edges[3] = InputEdge{5, false, 123458000ULL, 0, EdgeSource::kCleared};
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

// Re-encodes a parsed BEVE value.
void Emit(const beve::Value& value, beve::Writer& out) {
  switch (value.kind) {
    case beve::Kind::kString:
      out.String(value.text);
      break;
    case beve::Kind::kUnsigned:
      out.U64(value.number);
      break;
    case beve::Kind::kObject:
      out.BeginObject(value.fields.size());
      for (const beve::Field& field : value.fields) {
        out.Key(field.key);
        Emit(field.value, out);
      }
      break;
    case beve::Kind::kU8Array:
      out.Bytes(value.raw);
      break;
    case beve::Kind::kU64Array:
      out.U64Array(value.U64s());
      break;
  }
}

// |replay| with its top-level field |key| written by |write| instead: a
// well-formed BEVE file whose replay content may be corrupt.
std::vector<std::byte> WithField(
    std::span<const std::byte> replay, std::string_view key,
    const std::function<void(beve::Writer&)>& write) {
  const auto root = beve::Parse(replay);
  REQUIRE(root.has_value());
  beve::Writer out;
  out.BeginObject(root->fields.size());
  for (const beve::Field& field : root->fields) {
    out.Key(field.key);
    if (field.key == key) {
      write(out);
    } else {
      Emit(field.value, out);
    }
  }
  return out.Take();
}

// Open() fails with a replay error and does not throw.
void RequireRejected(const std::vector<std::byte>& file, std::string_view why) {
  INFO(why);
  bool opened = true;
  std::string message;
  CHECK_NOTHROW([&] {
    const auto reader = ReplayReader::Open(file);
    opened = reader.has_value();
    if (!opened) message = reader.error().message;
  }());
  REQUIRE_FALSE(opened);
  CHECK(message.starts_with("replay: "));
}

TEST_CASE("A corrupt replay is an error, not a throw", "[replay]") {
  ReplayRecorder recorder;
  recorder.Begin(MakeReplayHeader({}, 1, 120), {});
  for (Tick t = 1; t <= 3; ++t) recorder.Record(SampleFrame(t));
  const std::vector<std::byte> bytes = recorder.Finish();
  const auto parsed = beve::Parse(bytes);
  REQUIRE(parsed.has_value());
  const std::span<const std::byte> frames = parsed->Find("frames")->raw;

  auto with_count = [&bytes](std::uint64_t n) {
    return WithField(bytes, "frame_count",
                     [n](beve::Writer& out) { out.U64(n); });
  };
  auto with_frames = [&bytes](const std::vector<std::byte>& raw) {
    return WithField(bytes, "frames",
                     [&raw](beve::Writer& out) { out.Bytes(raw); });
  };

  // Rewriting keeps a good file good, so each failure below is the field's.
  REQUIRE(ReplayReader::Open(with_count(3)).has_value());
  REQUIRE(ReplayReader::Open(with_frames({frames.begin(), frames.end()}))
              .has_value());

  RequireRejected(with_count(1ULL << 62), "a count no allocation can hold");
  RequireRejected(with_count(~0ULL), "the largest count");
  RequireRejected(with_count(4), "one frame more than the bytes hold");
  RequireRejected(with_count(2), "one frame fewer than the bytes hold");
  RequireRejected(with_count(0), "no frames counted");
  const std::vector<std::byte> cut(frames.begin(), frames.end() - 5);
  RequireRejected(with_frames(cut), "a truncated last frame");
  std::vector<std::byte> trailing(frames.begin(), frames.end());
  trailing.insert(trailing.end(), 3, std::byte{0});
  RequireRejected(with_frames(trailing), "bytes after the last frame");
  std::vector<std::byte> doubled(frames.begin(), frames.end());
  doubled.insert(doubled.end(), frames.begin(), frames.end());
  RequireRejected(with_frames(doubled), "more frames than counted");
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

  // The replay's session log is the whole log: extra edits are refused,
  // not silently dropped.
  testing::KernelSessionOptions both = replay;
  both.edits_path = testing::EditsFixturePath();
  const testing::KernelSessionResult refused = testing::RunKernelSession(both);
  CHECK(refused.error.find("edits") != std::string::npos);
  CHECK(refused.hashes.empty());
}

}  // namespace
}  // namespace se
