// Copyright 2026 The simple-engine-2 Authors.
//
// se_replay --from-dump's reader and replay (docs/milestone-specs.md M12) on
// synthetic minidumps: the real dump-stream regions (an InputFrameRing, a
// SessionHeaderStream, a breadcrumb table) filled from a headless rampage
// session, laid out in a minidump by WriteMinidump. Runs everywhere; the
// desktop test crash.rampage_hang replays a real hang dump.

#include "se_replay/dump_session.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rampage/support/rampage_session.h"
#include "rampage/support/seinput.h"
#include "se_core/breadcrumb.h"
#include "se_core/dump_stream.h"
#include "se_core/input_frame.h"
#include "se_core/input_frame_ring.h"
#include "se_core/schedule.h"
#include "se_core/session_header.h"
#include "se_core/sim_clock.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_sim/replay.h"
#include "support/test_roles.h"

namespace se::replay {
namespace {

constexpr char kScene[] = "assets/scenes/sandbox.scene.json";
constexpr std::uint64_t kSeed = 1;

std::vector<std::byte> SceneBytes() {
  std::ifstream in(testing::RampageRepoRoot() + "/" + kScene, std::ios::binary);
  const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return bytes;
}

// The tests stand in for the World owner.
void WriteAsOwner(InputFrameRing& ring,
                  const InputFrame& frame) SE_NO_THREAD_SAFETY_ANALYSIS {
  ring.Write(frame);
}

std::span<const std::byte> Bytes(const std::vector<std::byte>& bytes) {
  return {bytes.data(), bytes.size()};
}

// A dumped session: |frames| scripted InputFrames in the ring after |before|
// frames of an earlier session, main's breadcrumb at |crumb_tick|, and tick
// |published|'s hash (of |hashes|, the session's own) in the header.
struct SyntheticDump {
  std::unique_ptr<InputFrameRing> ring = std::make_unique<InputFrameRing>();
  std::unique_ptr<SessionHeaderStream> header =
      std::make_unique<SessionHeaderStream>();
  std::vector<std::byte> breadcrumbs;
  std::vector<InputFrame> frames;

  SyntheticDump(Tick ticks, std::uint64_t before) {
    for (std::uint64_t i = 0; i < before; ++i) {
      InputFrame stale;
      stale.tick = i + 1;
      WriteAsOwner(*ring, stale);
    }
    const ReplayHeader replay =
        MakeReplayHeader(SceneBytes(), kSeed, kDefaultTickRate);
    SessionHeaderInfo info;
    info.module = "rampage";
    info.scene = kScene;
    info.scene_hash = replay.scene_hash;
    info.seed = kSeed;
    info.tick_rate = kDefaultTickRate;
    info.build = replay.build;
    info.platform = replay.platform;
    info.compiler = replay.compiler;
    info.math_hash = replay.math_hash;
    info.cpu_features = replay.cpu_features;
    info.start_tick = 1;
    info.first_frame = ring->Head();
    header->Begin(info);
    for (Tick tick = 1; tick <= ticks; ++tick) {
      frames.push_back(testing::RampageScriptedInput::FrameFor(tick, 9));
      WriteAsOwner(*ring, frames.back());
    }
  }

  void Breadcrumb(Tick tick) {
    breadcrumbs.assign(sizeof(DumpStreamHeader) + 64 * sizeof(BreadcrumbRecord),
                       std::byte{0});
    DumpStreamHeader stream;
    InitDumpStreamHeader(stream, DumpStreamKind::kBreadcrumbs,
                         sizeof(BreadcrumbRecord), 64, "threads");
    std::memcpy(breadcrumbs.data(), &stream, sizeof(stream));
    BreadcrumbRecord main;
    main.packed =
        PackBreadcrumb({.role = static_cast<std::uint8_t>(ThreadRole::kMain),
                        .phase = static_cast<std::uint8_t>(Phase::kCleanup),
                        .system = 9,
                        .tick = static_cast<std::uint32_t>(tick)});
    main.role = static_cast<std::uint8_t>(ThreadRole::kMain);
    main.state = 1;
    std::memcpy(breadcrumbs.data() + sizeof(DumpStreamHeader), &main,
                sizeof(main));
  }

  [[nodiscard]] std::vector<std::byte> Minidump(
      std::span<const std::byte> ring_bytes) const {
    std::vector<MinidumpStream> streams = {
        {0x53450002, ring_bytes},
        {0x53450005, header->StreamBytes()},
    };
    if (!breadcrumbs.empty()) {
      streams.push_back({0x53450001, Bytes(breadcrumbs)});
    }
    return WriteMinidump(streams);
  }
  [[nodiscard]] std::vector<std::byte> Minidump() const {
    return Minidump(ring->StreamBytes());
  }
};

// The hashes of a headless run of the same frames.
std::vector<std::uint64_t> ReferenceHashes(
    const std::vector<InputFrame>& frames) SE_REQUIRES(g_main)
    SE_EXCLUDES(g_sim_owner) {
  testing::RampageSessionOptions options;
  options.scene = testing::SandboxSceneRequest();
  options.seed = kSeed;
  options.ticks = frames.size();
  testing::FrameListInput input(frames);
  options.input = &input;
  const testing::RampageSessionResult result =
      testing::RunRampageSession(options);
  REQUIRE(result.error.empty());
  return result.hashes;
}

TEST_CASE("A dump of a hang replays to main's breadcrumb tick", "[from_dump]") {
  const testing::MainThreadScope main_thread;
  constexpr Tick kHang = 120;
  // An earlier session's frames are in the ring first (an earlier App in
  // the process); the header says where this one's begin.
  SyntheticDump dump(kHang, 37);
  const std::vector<std::uint64_t> reference = ReferenceHashes(dump.frames);
  // Main is inside tick 120: its frame is in the ring, the last hash
  // published is tick 118's (the frame before ran two ticks).
  dump.Breadcrumb(kHang);
  dump.header->PublishTick(kHang - 2, reference[kHang - 3]);
  const std::vector<std::byte> bytes = dump.Minidump();

  auto session = ReadDumpedSession(Bytes(bytes));
  REQUIRE(session.has_value());
  REQUIRE(session->header.session == 1);
  REQUIRE(session->header.first_frame == 37);
  REQUIRE(session->frames.size() == kHang);
  REQUIRE(session->frames.front().tick == 1);
  CHECK(session->frames.back().tick == kHang);
  REQUIRE(session->main_live);
  CHECK(session->main.tick == kHang);
  CHECK(session->main.phase == static_cast<std::uint8_t>(Phase::kCleanup));

  const FromDumpResult result =
      ReplayDumpedRampage(*session, testing::RampageRepoRoot());
  REQUIRE(result.error.empty());
  CHECK(result.breadcrumb_tick == kHang);
  CHECK(result.inside_tick);
  REQUIRE(result.last_tick == kHang - 1);
  REQUIRE(result.hashes.size() == kHang - 1);
  CHECK(result.checked_tick == kHang - 2);
  for (std::size_t i = 0; i < result.hashes.size(); ++i) {
    REQUIRE(result.hashes[i] == reference[i]);
  }
}

TEST_CASE("A dump after a finished tick replays through it", "[from_dump]") {
  const testing::MainThreadScope main_thread;
  constexpr Tick kTick = 60;
  SyntheticDump dump(kTick, 0);
  const std::vector<std::uint64_t> reference = ReferenceHashes(dump.frames);
  dump.Breadcrumb(kTick);
  dump.header->PublishTick(kTick, reference[kTick - 1]);
  auto session = ReadDumpedSession(Bytes(dump.Minidump()));
  REQUIRE(session.has_value());
  const FromDumpResult result =
      ReplayDumpedRampage(*session, testing::RampageRepoRoot());
  REQUIRE(result.error.empty());
  CHECK_FALSE(result.inside_tick);
  CHECK(result.last_tick == kTick);
  CHECK(result.checked_tick == kTick);
}

TEST_CASE("A hash or a scene that differs from the dump's is reported",
          "[from_dump]") {
  const testing::MainThreadScope main_thread;
  SyntheticDump dump(30, 0);
  dump.Breadcrumb(30);
  dump.header->PublishTick(29, 0x1234);
  auto session = ReadDumpedSession(Bytes(dump.Minidump()));
  REQUIRE(session.has_value());
  FromDumpResult result =
      ReplayDumpedRampage(*session, testing::RampageRepoRoot());
  CHECK(result.error.find("tick 29 replays to hash") != std::string::npos);
  REQUIRE(result.hashes.size() == 29);

  session->header.scene_hash ^= 1;
  result = ReplayDumpedRampage(*session, testing::RampageRepoRoot());
  CHECK(result.error.find("differs from the dumped session's scene") !=
        std::string::npos);
  CHECK(result.hashes.empty());
}

TEST_CASE("The reader rejects what it cannot replay", "[from_dump]") {
  const std::vector<std::byte> junk(256, std::byte{0x41});
  CHECK_FALSE(ReadDumpedSession(Bytes(junk)).has_value());

  // A session whose first frames the ring no longer holds.
  SyntheticDump long_session(kInputFrameRingCapacity + 10, 0);
  auto gone = ReadDumpedSession(Bytes(long_session.Minidump()));
  REQUIRE_FALSE(gone.has_value());
  CHECK(gone.error().message.find("are gone") != std::string::npos);

  // A torn slot inside the session is an error; the newest slot torn (in
  // progress) only ends the frames.
  SyntheticDump dump(20, 0);
  std::vector<std::byte> ring(dump.ring->StreamBytes().begin(),
                              dump.ring->StreamBytes().end());
  const auto set_stamp = [&ring](std::uint64_t index, std::uint64_t stamp) {
    std::memcpy(
        ring.data() + sizeof(DumpStreamHeader) + index * sizeof(InputFrameSlot),
        &stamp, sizeof(stamp));
  };
  set_stamp(19, 2 * 19 + 1);
  auto newest = ReadDumpedSession(Bytes(dump.Minidump(Bytes(ring))));
  REQUIRE(newest.has_value());
  REQUIRE(newest->frames.size() == 19);
  set_stamp(5, 2 * 5 + 1);
  CHECK_FALSE(ReadDumpedSession(Bytes(dump.Minidump(Bytes(ring)))).has_value());

  // No session begun.
  SyntheticDump fresh(0, 0);
  auto empty_header = std::make_unique<SessionHeaderStream>();
  const std::vector<MinidumpStream> streams = {
      {0x53450002, fresh.ring->StreamBytes()},
      {0x53450005, empty_header->StreamBytes()}};
  CHECK_FALSE(ReadDumpedSession(Bytes(WriteMinidump(streams))).has_value());
}

}  // namespace
}  // namespace se::replay
