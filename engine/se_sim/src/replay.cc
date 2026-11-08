// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/replay.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/assert.h"
#include "se_core/detmath.h"
#include "se_core/error.h"
#include "se_core/fp.h"
#include "se_core/hash.h"
#include "se_core/input_frame.h"
#include "se_sim/beve.h"
#include "se_sim/bytes.h"

#ifndef SE_BUILD_NAME
#define SE_BUILD_NAME "unknown"
#endif

namespace se {
namespace {

constexpr std::string_view kFormat = "se-replay";
constexpr std::uint64_t kVersion = 1;
constexpr std::uint64_t kSceneSeed = 0x5CE7E5EED0000001ULL;

std::string PlatformName() {
#if defined(_WIN32)
  return "windows";
#elif defined(__linux__)
  return "linux";
#elif defined(__APPLE__)
  return "macos";
#else
  return "unknown";
#endif
}

std::string CompilerName() {
#if defined(__clang__) && defined(_MSC_VER)
  return "clang-cl " + std::to_string(__clang_major__) + "." +
         std::to_string(__clang_minor__);
#elif defined(__clang__)
  return "clang " + std::to_string(__clang_major__) + "." +
         std::to_string(__clang_minor__);
#elif defined(_MSC_VER)
  return "msvc " + std::to_string(_MSC_FULL_VER);
#elif defined(__GNUC__)
  return "gcc " + std::to_string(__GNUC__) + "." +
         std::to_string(__GNUC_MINOR__);
#else
  return "unknown";
#endif
}

void EncodeFrame(const InputFrame& f, ByteWriter& out) {
  out.U64(f.tick);
  out.U64(f.held);
  for (const std::uint8_t p : f.pressed) out.U8(p);
  for (const std::uint8_t r : f.released) out.U8(r);
  for (const float a : f.axes) out.U32(std::bit_cast<std::uint32_t>(a));
  out.U8(f.platform_live ? 1 : 0);
  out.U8(f.edge_count);
  for (std::uint8_t i = 0; i < f.edge_count; ++i) {
    const InputEdge& e = f.edges[i];
    out.U8(e.action);
    out.U8(e.down ? 1 : 0);
    out.U64(e.stamp_ns);
    out.U8(e.order);
    out.U8(static_cast<std::uint8_t>(e.source));
  }
}

bool DecodeFrame(ByteReader& in, InputFrame* f) {
  std::uint8_t live = 0;
  if (!in.U64(&f->tick) || !in.U64(&f->held)) return false;
  for (std::uint8_t& p : f->pressed) {
    if (!in.U8(&p)) return false;
  }
  for (std::uint8_t& r : f->released) {
    if (!in.U8(&r)) return false;
  }
  for (float& a : f->axes) {
    std::uint32_t bits = 0;
    if (!in.U32(&bits)) return false;
    a = std::bit_cast<float>(bits);
  }
  if (!in.U8(&live) || !in.U8(&f->edge_count)) return false;
  if (f->edge_count > kMaxEdgesPerTick) return false;
  f->platform_live = live != 0;
  for (std::uint8_t i = 0; i < f->edge_count; ++i) {
    InputEdge& e = f->edges[i];
    std::uint8_t down = 0;
    std::uint8_t source = 0;
    if (!in.U8(&e.action) || !in.U8(&down) || !in.U64(&e.stamp_ns) ||
        !in.U8(&e.order) || !in.U8(&source) ||
        source > static_cast<std::uint8_t>(EdgeSource::kReplay)) {
      return false;
    }
    e.down = down != 0;
    e.source = static_cast<EdgeSource>(source);
  }
  return true;
}

std::unexpected<Error> Corrupt(std::string message) {
  return Fail(ErrorCode::kCorrupt, "replay: " + std::move(message));
}

}  // namespace

ReplayHeader MakeReplayHeader(std::span<const std::byte> start_bytes,
                              std::uint64_t seed, std::uint32_t tick_rate) {
  ReplayHeader header;
  header.build = SE_BUILD_NAME;
  header.platform = PlatformName();
  header.compiler = CompilerName();
  header.cpu_features = fp::CpuFeatureBits();
  header.math_hash = dm::SelfTestHash();
  Hasher scene(kSceneSeed);
  scene.AddBytes(start_bytes);
  header.scene_hash = scene.Finish();
  header.seed = seed;
  header.tick_rate = tick_rate;
  return header;
}

void ReplayRecorder::Begin(const ReplayHeader& header,
                           std::span<const std::byte> start) {
  *this = ReplayRecorder();
  active_ = true;
  header_ = header;
  start_.assign(start.begin(), start.end());
}

void ReplayRecorder::Record(const InputFrame& frame) {
  if (!active_) return;
  if (frames_ == 0) first_tick_ = frame.tick;
  SE_ASSERT_MSG(frame.tick == first_tick_ + frames_,
                "replay frames must be consecutive");
  ByteWriter out;
  EncodeFrame(frame, out);
  const std::vector<std::byte>& bytes = out.Data();
  frame_bytes_.insert(frame_bytes_.end(), bytes.begin(), bytes.end());
  ++frames_;
}

void ReplayRecorder::RecordHash(Tick tick, std::uint64_t hash) {
  if (!active_) return;
  if (hashes_.empty()) first_hash_tick_ = tick;
  SE_ASSERT_MSG(tick == first_hash_tick_ + hashes_.size(),
                "replay hashes must be consecutive");
  hashes_.push_back(hash);
}

void ReplayRecorder::RecordSession(const SessionLog& log) {
  if (!active_) return;
  session_ = log.Write();
}

std::vector<std::byte> ReplayRecorder::Finish() {
  beve::Writer w;
  w.BeginObject(10);
  w.Key("format");
  w.String(kFormat);
  w.Key("version");
  w.U64(kVersion);
  w.Key("header");
  w.BeginObject(8);
  w.Key("build");
  w.String(header_.build);
  w.Key("platform");
  w.String(header_.platform);
  w.Key("compiler");
  w.String(header_.compiler);
  w.Key("cpu_features");
  w.U64(header_.cpu_features);
  w.Key("math_hash");
  w.U64(header_.math_hash);
  w.Key("scene_hash");
  w.U64(header_.scene_hash);
  w.Key("seed");
  w.U64(header_.seed);
  w.Key("tick_rate");
  w.U32(header_.tick_rate);
  w.Key("start");
  w.Bytes(start_);
  w.Key("first_tick");
  w.U64(first_tick_);
  w.Key("frame_count");
  w.U64(frames_);
  w.Key("frames");
  w.Bytes(frame_bytes_);
  w.Key("first_hash_tick");
  w.U64(first_hash_tick_);
  w.Key("hashes");
  w.U64Array(hashes_);
  w.Key("session");
  w.Bytes(session_);
  active_ = false;
  return w.Take();
}

std::expected<ReplayReader, Error> ReplayReader::Open(
    std::span<const std::byte> bytes) {
  auto parsed = beve::Parse(bytes);
  if (!parsed) return std::unexpected(parsed.error());
  const beve::Value& root = *parsed;
  auto text = [&](const beve::Value* v) {
    return v != nullptr && v->kind == beve::Kind::kString;
  };
  auto number = [&](const beve::Value* v) {
    return v != nullptr && v->kind == beve::Kind::kUnsigned;
  };
  auto bytes_of = [&](const beve::Value* v) {
    return v != nullptr && v->kind == beve::Kind::kU8Array;
  };
  const beve::Value* format = root.Find("format");
  if (!text(format) || format->text != kFormat) {
    return Corrupt("not an se-replay file");
  }
  const beve::Value* version = root.Find("version");
  if (!number(version) || version->number != kVersion) {
    return Fail(ErrorCode::kVersion, "replay: unsupported version");
  }
  const beve::Value* header = root.Find("header");
  if (header == nullptr || header->kind != beve::Kind::kObject) {
    return Corrupt("missing header");
  }
  ReplayReader reader;
  ReplayHeader& h = reader.header_;
  const beve::Value* build = header->Find("build");
  const beve::Value* platform = header->Find("platform");
  const beve::Value* compiler = header->Find("compiler");
  const beve::Value* cpu = header->Find("cpu_features");
  const beve::Value* math = header->Find("math_hash");
  const beve::Value* scene = header->Find("scene_hash");
  const beve::Value* seed = header->Find("seed");
  const beve::Value* rate = header->Find("tick_rate");
  if (!text(build) || !text(platform) || !text(compiler) || !number(cpu) ||
      !number(math) || !number(scene) || !number(seed) || !number(rate)) {
    return Corrupt("incomplete header");
  }
  h.build = std::string(build->text);
  h.platform = std::string(platform->text);
  h.compiler = std::string(compiler->text);
  h.cpu_features = cpu->number;
  h.math_hash = math->number;
  h.scene_hash = scene->number;
  h.seed = seed->number;
  h.tick_rate = static_cast<std::uint32_t>(rate->number);

  const beve::Value* start = root.Find("start");
  const beve::Value* first_tick = root.Find("first_tick");
  const beve::Value* frame_count = root.Find("frame_count");
  const beve::Value* frames = root.Find("frames");
  const beve::Value* first_hash = root.Find("first_hash_tick");
  const beve::Value* hashes = root.Find("hashes");
  const beve::Value* session = root.Find("session");
  if (!bytes_of(start) || !number(first_tick) || !number(frame_count) ||
      !bytes_of(frames) || !number(first_hash) || hashes == nullptr ||
      hashes->kind != beve::Kind::kU64Array || !bytes_of(session)) {
    return Corrupt("missing fields");
  }
  reader.start_.assign(start->raw.begin(), start->raw.end());
  reader.first_tick_ = first_tick->number;
  ByteReader in(frames->raw);
  reader.frames_.resize(static_cast<std::size_t>(frame_count->number));
  for (InputFrame& frame : reader.frames_) {
    if (!DecodeFrame(in, &frame)) return Corrupt("bad frame");
  }
  if (!in.AtEnd()) return Corrupt("trailing frame bytes");
  for (std::size_t i = 0; i < reader.frames_.size(); ++i) {
    if (reader.frames_[i].tick != reader.first_tick_ + i) {
      return Corrupt("frames are not consecutive");
    }
  }
  reader.first_hash_tick_ = first_hash->number;
  reader.hashes_ = hashes->U64s();
  reader.session_.assign(session->raw.begin(), session->raw.end());
  return reader;
}

InputFrame ReplayReader::Latch(Tick tick, std::uint64_t /*tick_window_end_ns*/,
                               bool /*last_tick_of_frame*/) {
  if (tick >= first_tick_ && tick - first_tick_ < frames_.size()) {
    return frames_[static_cast<std::size_t>(tick - first_tick_)];
  }
  InputFrame empty;
  empty.tick = tick;
  return empty;
}

bool ReplayReader::MathMatches() const noexcept {
  return header_.math_hash == dm::SelfTestHash();
}

}  // namespace se
