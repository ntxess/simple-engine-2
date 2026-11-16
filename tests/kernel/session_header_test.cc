// Copyright 2026 The simple-engine-2 Authors.
//
// SessionHeaderStream (M12): the kind-6 dump stream a replay from a dump
// starts from; its fields, truncation, and a reader thread that never takes
// a torn record (TSan in WSL).

#include "se_core/session_header.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "se_core/dump_stream.h"

namespace se {
namespace {

SessionHeaderInfo Info() {
  SessionHeaderInfo info;
  info.module = "rampage";
  info.scene = "assets/scenes/sandbox.scene.json";
  info.scene_hash = 0x0123456789ABCDEF;
  info.seed = 1;
  info.tick_rate = 120;
  info.build = "simple-engine 2.0 Dev";
  info.platform = "windows";
  info.compiler = "msvc";
  info.math_hash = 0xFB93;
  info.cpu_features = 0x7;
  info.start_tick = 1;
  info.first_frame = 37;
  return info;
}

template <std::size_t N>
std::string_view Text(const char (&field)[N]) {
  std::size_t size = 0;
  while (size < N && field[size] != '\0') ++size;
  return {field, size};
}

TEST_CASE("The session header is a kind-6 dump stream", "[session_header]") {
  auto header = std::make_unique<SessionHeaderStream>();
  SessionHeaderRecord record;
  CHECK_FALSE(header->Read(record));
  REQUIRE(header->Sessions() == 0);
  const auto region = header->StreamBytes();
  REQUIRE(region.size() ==
          sizeof(DumpStreamHeader) + sizeof(SessionHeaderRecord));
  DumpStreamHeader stream;
  std::memcpy(&stream, region.data(), sizeof(stream));
  CHECK(stream.magic == kDumpStreamMagic);
  CHECK(stream.kind ==
        static_cast<std::uint16_t>(DumpStreamKind::kSessionHeader));
  CHECK(stream.record_size == sizeof(SessionHeaderRecord));
  REQUIRE(stream.capacity == 1);
  REQUIRE(stream.head == 0);
  REQUIRE(std::string_view(stream.name) == "session-header");

  header->Begin(Info());
  REQUIRE(header->Read(record));
  REQUIRE(record.session == 1);
  REQUIRE(record.stamp % 2 == 0);
  REQUIRE(Text(record.module) == "rampage");
  REQUIRE(Text(record.scene) == "assets/scenes/sandbox.scene.json");
  REQUIRE(Text(record.build) == "simple-engine 2.0 Dev");
  REQUIRE(Text(record.platform) == "windows");
  REQUIRE(Text(record.compiler) == "msvc");
  REQUIRE(record.scene_hash == 0x0123456789ABCDEF);
  REQUIRE(record.seed == 1);
  REQUIRE(record.tick_rate == 120);
  REQUIRE(record.math_hash == 0xFB93);
  REQUIRE(record.cpu_features == 0x7);
  REQUIRE(record.start_tick == 1);
  REQUIRE(record.first_frame == 37);
  REQUIRE(record.last_tick == 0);
  std::memcpy(&stream, region.data(), sizeof(stream));
  REQUIRE(stream.head == 1);
  // The region holds the same record the reader returned.
  SessionHeaderRecord raw;
  std::memcpy(&raw, region.data() + sizeof(DumpStreamHeader), sizeof(raw));
  REQUIRE(std::memcmp(&raw, &record, sizeof(raw)) == 0);

  header->PublishTick(240, 0xFEED);
  REQUIRE(header->Read(record));
  REQUIRE(record.last_tick == 240);
  REQUIRE(record.last_hash == 0xFEED);
  REQUIRE(record.first_frame == 37);

  // A second session starts over; long text is cut to its field.
  SessionHeaderInfo second = Info();
  const std::string long_scene(300, 's');
  second.scene = long_scene;
  header->Begin(second);
  REQUIRE(header->Read(record));
  REQUIRE(record.session == 2);
  REQUIRE(record.last_tick == 0);
  CHECK(Text(record.scene) == std::string(kSessionSceneSize, 's'));
  REQUIRE(header->Sessions() == 2);
}

TEST_CASE("A reader thread never takes a torn session header",
          "[session_header][threads]") {
  auto header = std::make_unique<SessionHeaderStream>();
  header->Begin(Info());
  constexpr std::uint64_t kTicks = 100'000;
  constexpr std::uint64_t kMix = 0x9E3779B97F4A7C15ULL;
  std::atomic<bool> started{false};
  std::atomic<bool> done{false};
  std::uint64_t accepted = 0;
  std::uint64_t mismatched = 0;
  std::thread reader([&] {
    SessionHeaderRecord record;
    started.store(true, std::memory_order_release);
    for (;;) {
      // One more read after the writer is done: it must be whole.
      const bool last = done.load(std::memory_order_acquire);
      if (header->Read(record)) {
        ++accepted;
        if (record.last_hash != record.last_tick * kMix ||
            Text(record.module) != "rampage") {
          ++mismatched;
        }
      }
      if (last) break;
    }
  });
  while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
  for (std::uint64_t tick = 1; tick <= kTicks; ++tick) {
    header->PublishTick(tick, tick * kMix);
    if (tick % 10'000 == 0) header->Begin(Info());
    if (tick % 1'000 == 0) std::this_thread::yield();
  }
  done.store(true, std::memory_order_release);
  reader.join();
  REQUIRE(accepted > 0);
  REQUIRE(mismatched == 0);
}

}  // namespace
}  // namespace se
