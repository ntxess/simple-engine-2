// Copyright 2026 The simple-engine-2 Authors.

#include "se_replay/dump_session.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/flow.h"
#include "game_rampage/module.h"
#include "rampage/support/rampage_session.h"
#include "rampage/support/seinput.h"
#include "se_core/breadcrumb.h"
#include "se_core/dump_stream.h"
#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/sim_clock.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_sim/content_bundle.h"
#include "se_sim/game_module.h"
#include "se_sim/replay.h"
#include "support/content_test_module.h"
#include "support/module_session.h"

namespace se::replay {
namespace {

constexpr std::uint32_t kMinidumpSignature = 0x504D444D;  // "MDMP"
constexpr std::uint32_t kMinidumpVersion = 0xA793;
constexpr std::size_t kMinidumpHeaderSize = 32;
constexpr std::size_t kDirectoryEntrySize = 12;
// The first-party stream types (se_crash/crash.h).
constexpr std::uint32_t kBreadcrumbsStream = 0x53450001;
constexpr std::uint32_t kInputFrameRingStream = 0x53450002;
constexpr std::uint32_t kSessionHeaderStream = 0x53450005;
constexpr std::uint32_t kPlayStartStream = 0x53450006;
constexpr std::uint64_t kTickWrap = kBreadcrumbTickMask + 1;
constexpr std::uint8_t kSlotLive = 1;

std::unexpected<Error> Corrupt(std::string message) {
  return Fail(ErrorCode::kCorrupt, "dump: " + std::move(message));
}

template <class T>
bool ReadAt(std::span<const std::byte> bytes, std::size_t offset, T& out) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;
  std::memcpy(&out, bytes.data() + offset, sizeof(T));
  return true;
}

template <class T>
void Append(std::vector<std::byte>& out, const T& value) {
  const auto* first = reinterpret_cast<const std::byte*>(&value);
  out.insert(out.end(), first, first + sizeof(T));
}

template <std::size_t N>
std::string_view Text(const char (&field)[N]) {
  const char* end = std::find(field, field + N, '\0');
  return {field, static_cast<std::size_t>(end - field)};
}

// The stream directory: type to bytes (the first stream of each type).
std::expected<std::vector<MinidumpStream>, Error> ReadDirectory(
    std::span<const std::byte> dump) {
  std::uint32_t signature = 0;
  std::uint32_t version = 0;
  std::uint32_t count = 0;
  std::uint32_t directory = 0;
  if (!ReadAt(dump, 0, signature) || !ReadAt(dump, 4, version) ||
      !ReadAt(dump, 8, count) || !ReadAt(dump, 12, directory) ||
      signature != kMinidumpSignature ||
      (version & 0xFFFF) != kMinidumpVersion) {
    return Corrupt("not a minidump (no MDMP header)");
  }
  std::vector<MinidumpStream> streams;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t entry = directory + std::size_t{i} * kDirectoryEntrySize;
    std::uint32_t type = 0;
    std::uint32_t size = 0;
    std::uint32_t rva = 0;
    if (!ReadAt(dump, entry, type) || !ReadAt(dump, entry + 4, size) ||
        !ReadAt(dump, entry + 8, rva)) {
      return Corrupt("the stream directory runs past the end of the file");
    }
    if (rva > dump.size() || dump.size() - rva < size) {
      return Corrupt(std::format("stream {:#010x} runs past the end", type));
    }
    streams.push_back({type, dump.subspan(rva, size)});
  }
  return streams;
}

std::optional<std::span<const std::byte>> Find(
    const std::vector<MinidumpStream>& streams, std::uint32_t type) {
  for (const MinidumpStream& stream : streams) {
    if (stream.type == type) return stream.bytes;
  }
  return std::nullopt;
}

// The stream's header, checked against |kind| and |record_size|.
std::expected<DumpStreamHeader, Error> StreamHeader(
    std::span<const std::byte> bytes, DumpStreamKind kind,
    std::size_t record_size, std::string_view what) {
  DumpStreamHeader header;
  if (!ReadAt(bytes, 0, header) || header.magic != kDumpStreamMagic ||
      header.version != kDumpStreamVersion ||
      header.kind != static_cast<std::uint16_t>(kind)) {
    return Corrupt(std::format("the {} stream has no valid header", what));
  }
  if (header.record_size != record_size ||
      bytes.size() < sizeof(DumpStreamHeader) +
                         std::size_t{header.capacity} * header.record_size) {
    return Corrupt(
        std::format("the {} stream has records of {} bytes x {}, "
                    "{} bytes in all",
                    what, header.record_size, header.capacity, bytes.size()));
  }
  return header;
}

std::expected<void, Error> ReadFrames(std::span<const std::byte> ring,
                                      DumpedSession& session) {
  auto header = StreamHeader(ring, DumpStreamKind::kInputFrameRing,
                             sizeof(InputFrameSlot), "InputFrame ring");
  if (!header) return std::unexpected(std::move(header.error()));
  const std::uint64_t capacity = header->capacity;
  const std::uint64_t head = header->head;
  const std::uint64_t first = session.header.first_frame;
  session.ring_head = head;
  if (capacity == 0 || first > head) {
    return Corrupt(
        std::format("the InputFrame ring's head {} is before the "
                    "session's first frame {}",
                    head, first));
  }
  if (head - first > capacity) {
    return Corrupt(std::format(
        "the InputFrame ring holds frames {} to {}; the session's first {} "
        "frames are gone (the ring keeps {})",
        head - capacity, head - 1, head - capacity - first, capacity));
  }
  for (std::uint64_t index = first; index < head; ++index) {
    const std::size_t offset =
        sizeof(DumpStreamHeader) + (index % capacity) * sizeof(InputFrameSlot);
    std::uint64_t stamp = 0;
    InputFrame frame;
    if (!ReadAt(ring, offset, stamp) ||
        !ReadAt(ring, offset + offsetof(InputFrameSlot, frame), frame)) {
      return Corrupt("an InputFrame slot runs past the stream");
    }
    if (stamp != 2 * index + 2) {
      // The newest frame may have been in progress when the dump was taken.
      if (index + 1 == head) break;
      return Corrupt(
          std::format("InputFrame {} has stamp {} (torn)", index, stamp));
    }
    const Tick expected = session.header.start_tick + (index - first);
    if (frame.tick != expected) {
      return Corrupt(std::format("InputFrame {} carries tick {}, not {}", index,
                                 frame.tick, expected));
    }
    session.frames.push_back(frame);
  }
  return {};
}

void ReadMain(std::span<const std::byte> table, DumpedSession& session) {
  auto header = StreamHeader(table, DumpStreamKind::kBreadcrumbs,
                             sizeof(BreadcrumbRecord), "breadcrumb");
  if (!header || header->capacity == 0) return;
  BreadcrumbRecord main;
  if (!ReadAt(table, sizeof(DumpStreamHeader), main)) return;
  session.main_live = main.state == kSlotLive &&
                      main.role == static_cast<std::uint8_t>(ThreadRole::kMain);
  session.main = UnpackBreadcrumb(main.packed);
}

std::optional<std::vector<std::byte>> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return bytes;
}

}  // namespace

std::expected<DumpedSession, Error> ReadDumpedSession(
    std::span<const std::byte> minidump) {
  auto streams = ReadDirectory(minidump);
  if (!streams) return std::unexpected(std::move(streams.error()));
  DumpedSession session;
  const auto header_bytes = Find(*streams, kSessionHeaderStream);
  if (!header_bytes) return Corrupt("no session header stream (0x53450005)");
  auto header = StreamHeader(*header_bytes, DumpStreamKind::kSessionHeader,
                             sizeof(SessionHeaderRecord), "session header");
  if (!header) return std::unexpected(std::move(header.error()));
  if (header->head == 0 ||
      !ReadAt(*header_bytes, sizeof(DumpStreamHeader), session.header) ||
      session.header.session == 0) {
    return Corrupt("the session header was never begun");
  }
  if (session.header.stamp % 2 != 0) {
    return Corrupt("the session header was being rewritten (torn)");
  }
  const auto ring = Find(*streams, kInputFrameRingStream);
  if (!ring) return Corrupt("no InputFrame ring stream (0x53450002)");
  if (auto frames = ReadFrames(*ring, session); !frames) {
    return std::unexpected(std::move(frames.error()));
  }
  if (const auto table = Find(*streams, kBreadcrumbsStream)) {
    ReadMain(*table, session);
  }
  // A withdrawn Play-start stream is an entry without bytes.
  if (const auto start = Find(*streams, kPlayStartStream);
      start && !start->empty()) {
    auto bytes = StreamHeader(*start, DumpStreamKind::kBytes, 1, "Play-start");
    if (!bytes) return std::unexpected(std::move(bytes.error()));
    const std::span<const std::byte> payload =
        start->subspan(sizeof(DumpStreamHeader), bytes->capacity);
    session.play_start.assign(payload.begin(), payload.end());
  }
  return session;
}

std::vector<std::byte> WriteMinidump(std::span<const MinidumpStream> streams) {
  std::vector<std::byte> out;
  const auto count = static_cast<std::uint32_t>(streams.size());
  Append(out, kMinidumpSignature);
  Append(out, kMinidumpVersion);
  Append(out, count);
  Append(out, static_cast<std::uint32_t>(kMinidumpHeaderSize));
  Append(out, std::uint32_t{0});  // CheckSum
  Append(out, std::uint32_t{0});  // TimeDateStamp
  Append(out, std::uint64_t{0});  // Flags
  std::size_t data = kMinidumpHeaderSize + streams.size() * kDirectoryEntrySize;
  std::vector<std::uint32_t> rvas;
  for (const MinidumpStream& stream : streams) {
    data = (data + 7) & ~std::size_t{7};
    rvas.push_back(static_cast<std::uint32_t>(data));
    data += stream.bytes.size();
  }
  for (std::size_t i = 0; i < streams.size(); ++i) {
    Append(out, streams[i].type);
    Append(out, static_cast<std::uint32_t>(streams[i].bytes.size()));
    Append(out, rvas[i]);
  }
  for (std::size_t i = 0; i < streams.size(); ++i) {
    out.resize(rvas[i], std::byte{0});
    out.insert(out.end(), streams[i].bytes.begin(), streams[i].bytes.end());
  }
  return out;
}

bool PointGameplayAtBundle(const ContentBundle& bundle) {
  if (bundle.Latest(rampage::kPlayGameSceneKey) == nullptr) return false;
  rampage::FlowConfig flow = rampage::GetFlowConfig();
  flow.gameplay_scene = std::string(rampage::kPlayGameSceneKey);
  rampage::SetFlowConfig(std::move(flow));
  return true;
}

FromDumpResult ReplayDumpedRampage(const DumpedSession& session,
                                   const std::string& root,
                                   const std::string& start_scene,
                                   const std::string& start_state) {
  FromDumpResult result;
  const SessionHeaderRecord& header = session.header;
  if ((header.flags & kSessionFlagNoSession) != 0) {
    result.error =
        "the dump holds no running session (the editor was in Edit mode): "
        "nothing to replay";
    return result;
  }
  const GameModule* module = nullptr;
  if (Text(header.module) == "rampage") {
    module = &rampage::RampageModule();
  } else if (Text(header.module) == "content_test") {
    module = &testing::ContentTestModule();
  } else {
    result.error = std::format(
        "the dump's session runs module '{}'; --from-dump replays rampage "
        "(and the content test module)",
        Text(header.module));
    return result;
  }
  if (header.start_tick != 1 || header.tick_rate != kDefaultTickRate) {
    result.error = std::format(
        "the dump's session starts at tick {} at {} Hz; rampage sessions "
        "start at tick 1 at {} Hz",
        header.start_tick, header.tick_rate, kDefaultTickRate);
    return result;
  }
  // The scene: the game's file under the root, or the editor Play's start
  // bytes from the dump, with the SessionLog of the edits made during Play.
  const bool editor_play = (header.flags & kSessionFlagPlayStart) != 0;
  std::string scene = root + "/" + std::string(Text(header.scene));
  std::vector<std::byte> session_log;
  // M13: a game's session stream (the replay file without frames): its
  // SessionLog, and its start bytes when they are a content bundle.
  std::optional<ContentBundle> bundle;
  if (!editor_play && !session.play_start.empty()) {
    auto file = ReplayReader::Open(session.play_start);
    if (!file) {
      result.error =
          "the dump's session stream: " + std::move(file.error().message);
      return result;
    }
    const std::span<const std::byte> log = file->SessionBytes();
    session_log.assign(log.begin(), log.end());
    if (ContentBundle::IsBundle(file->StartBytes())) {
      auto read = ContentBundle::Read(file->StartBytes());
      if (!read) {
        result.error = "the dump's content bundle: " + read.error().message;
        return result;
      }
      bundle = std::move(*read);
    }
  }
  if (!bundle && module != &rampage::RampageModule()) {
    result.error = std::format(
        "a '{}' session replays only from a content bundle in its session "
        "stream",
        Text(header.module));
    return result;
  }
  if (editor_play) {
    if (session.play_start.empty()) {
      result.error =
          "the dump's session is an editor Play, but the dump has no "
          "Play-start stream (0x53450006)";
      return result;
    }
    auto file = ReplayReader::Open(session.play_start);
    if (!file) {
      result.error =
          "the dump's Play-start stream: " + std::move(file.error().message);
      return result;
    }
    const std::span<const std::byte> start = file->StartBytes();
    const std::span<const std::byte> log = file->SessionBytes();
    session_log.assign(log.begin(), log.end());
    if (ContentBundle::IsBundle(start)) {
      // 'Play game' (M13 integration): the stream starts from the content
      // bundle of every text its states read, the Gameplay scene (the
      // Play-start bytes the header hashes) under rampage's Play game key
      // (review fix F4).
      auto read = ContentBundle::Read(start);
      if (!read) {
        result.error = "the dump's content bundle: " + read.error().message;
        return result;
      }
      const ContentText* scene = read->Latest(rampage::kPlayGameSceneKey);
      if (scene != nullptr &&
          MakeReplayHeader(std::as_bytes(std::span<const char>(scene->text)),
                           header.seed, header.tick_rate)
                  .scene_hash != header.scene_hash) {
        result.error = std::format(
            "the dump's Play game scene differs from the dumped session's "
            "Play-start bytes (hash {:016x})",
            header.scene_hash);
        return result;
      }
      bundle = std::move(*read);
    }
  }
  if (editor_play && !bundle) {
    if (start_scene.empty()) {
      result.error =
          "an editor Play session needs a file for its Play-start "
          "bytes (--start-scene)";
      return result;
    }
    auto file = ReplayReader::Open(session.play_start);
    if (!file) {
      result.error =
          "the dump's Play-start stream: " + std::move(file.error().message);
      return result;
    }
    const std::span<const std::byte> start = file->StartBytes();
    std::ofstream out(start_scene, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(start.data()),
              static_cast<std::streamsize>(start.size()));
    out.close();
    if (!out) {
      result.error = "cannot write the Play-start bytes to " + start_scene;
      return result;
    }
    scene = start_scene;
  }
  const std::optional<std::vector<std::byte>> bytes =
      bundle ? std::optional<std::vector<std::byte>>(std::vector<std::byte>())
             : ReadFile(scene);
  if (!bytes) {
    result.error = "cannot read the session's scene " + scene;
    return result;
  }
  // A bundle carries its own texts (each checked against its hash); a
  // scene file must be the one the session started from.
  if (!bundle &&
      MakeReplayHeader(*bytes, header.seed, header.tick_rate).scene_hash !=
          header.scene_hash) {
    result.error =
        std::format("{} differs from the dumped session's scene (hash {:016x})",
                    scene, header.scene_hash);
    return result;
  }
  // Main's breadcrumb holds the tick modulo 2^28; the frames give the
  // rest.
  const Tick last_frame =
      session.frames.empty() ? 0 : session.frames.back().tick;
  Tick target = last_frame;
  if (session.main_live) {
    target = (last_frame & ~kBreadcrumbTickMask) | session.main.tick;
    if (target > last_frame && target >= kTickWrap) target -= kTickWrap;
  }
  result.breadcrumb_tick = target;
  // A tick whose hash was published had finished; otherwise main was
  // inside it.
  result.inside_tick = session.main_live && header.last_tick < target;
  result.last_tick = result.inside_tick ? target - 1 : target;
  if (result.last_tick > session.frames.size()) {
    result.error = std::format(
        "the dump's InputFrame ring holds ticks 1 to {}; the replay to tick "
        "{} needs them up to {}",
        session.frames.size(), target, result.last_tick);
    return result;
  }
  if (bundle) {
    if (module == &rampage::RampageModule()) PointGameplayAtBundle(*bundle);
    ContentBundleSource source(std::move(*bundle));
    testing::FrameListInput frames(session.frames);
    testing::ModuleSessionOptions run;
    run.module = module;
    run.start_state =
        !start_state.empty() ? start_state : source.Bundle().StartState();
    run.bundle = &source;
    run.seed = header.seed;
    run.tick_rate = header.tick_rate;
    run.ticks = result.last_tick;
    run.input = &frames;
    run.session = std::move(session_log);
    testing::ModuleSessionResult replayed = testing::RunModuleSession(run);
    if (!replayed.error.empty()) {
      result.error = std::move(replayed.error);
      return result;
    }
    result.hashes = std::move(replayed.hashes);
    result.top_states = std::move(replayed.top_states);
  } else {
    testing::RampageSessionOptions options;
    options.scene.format = rampage::SceneRequest::Format::kScene;
    options.scene.path = scene;
    options.scene.asset_root = root + "/assets";
    options.scene.legacy_root = root;
    options.seed = header.seed;
    options.ticks = result.last_tick;
    options.session = std::move(session_log);
    testing::FrameListInput input(session.frames);
    options.input = &input;
    testing::RampageSessionResult run = testing::RunRampageSession(options);
    if (!run.error.empty()) {
      result.error = run.error;
      return result;
    }
    result.hashes = std::move(run.hashes);
  }
  if (header.last_tick >= 1 && header.last_tick <= result.hashes.size()) {
    result.checked_tick = header.last_tick;
    const std::uint64_t replayed = result.hashes[header.last_tick - 1];
    if (replayed != header.last_hash) {
      result.error = std::format(
          "tick {} replays to hash {:016x}; the dump published {:016x}",
          header.last_tick, replayed, header.last_hash);
    }
  }
  return result;
}

}  // namespace se::replay
