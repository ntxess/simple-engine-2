// Copyright 2026 The simple-engine-2 Authors.
//
// SessionHeaderStream: what a replay of the running session needs, as a
// kind-6 dump stream (docs/milestone-specs.md M12; the layout is
// SessionHeaderRecord in se_core/dump_stream.h): the game module, the scene
// and the hash of its bytes, the seed, the tick rate, the build, the first
// tick and where its InputFrame sits in the InputFrame ring, and the newest
// tick whose hash was published, so that tools/se_replay --from-dump can
// replay the session from a dump and check where it arrived.
//
//   SessionHeaderStream& header = ...;  // one per process, never destroyed
//   header.Begin(info);                 // boot step 10
//   header.PublishTick(tick, hash);     // after each frame's ticks
//
// One writer at a time (the App on main). The record carries a seqlock
// stamp (odd while it is rewritten) and is written word by word with
// relaxed atomics, so a reader on another thread (Read) or in the crash
// handler never takes a torn record for a whole one. A stream registered
// for dumps must never be destroyed: the handler may read it until the
// process exits.

#ifndef SE_CORE_SESSION_HEADER_H_
#define SE_CORE_SESSION_HEADER_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "se_core/dump_stream.h"
#include "se_core/types.h"

namespace se {

// Begin's fields; strings are truncated to their record fields.
struct SessionHeaderInfo {
  std::string_view module;
  // The scene file, relative to the content root (empty: none).
  std::string_view scene;
  std::uint64_t scene_hash = 0;
  std::uint64_t seed = 0;
  std::uint32_t tick_rate = 0;
  // From the replay header (se_sim/replay.h, MakeReplayHeader).
  std::string_view build;
  std::string_view platform;
  std::string_view compiler;
  std::uint64_t math_hash = 0;
  std::uint64_t cpu_features = 0;
  Tick start_tick = 1;
  // InputFrameRing::Head() when the session began.
  std::uint64_t first_frame = 0;
};

class SessionHeaderStream {
 public:
  SessionHeaderStream();
  ~SessionHeaderStream();

  SessionHeaderStream(const SessionHeaderStream&) = delete;
  SessionHeaderStream& operator=(const SessionHeaderStream&) = delete;

  // Starts a session: the whole record is rewritten, the session count
  // grows by one and no tick is published yet.
  void Begin(const SessionHeaderInfo& info) noexcept;
  // |tick|'s hash, the newest so far.
  void PublishTick(Tick tick, std::uint64_t hash) noexcept;

  // Sessions begun so far (any thread; 0 while a rewrite is in progress).
  [[nodiscard]] std::uint64_t Sessions() const noexcept;
  // A consistent copy of the record (any thread); false while it is being
  // rewritten or before the first Begin.
  [[nodiscard]] bool Read(SessionHeaderRecord& out) const noexcept;

  // The dump-stream region: the header and the record.
  [[nodiscard]] std::span<const std::byte> StreamBytes() const noexcept;

 private:
  static constexpr std::size_t kWords = sizeof(SessionHeaderRecord) / 8;
  static_assert(sizeof(SessionHeaderRecord) % 8 == 0);

  // Stores |record|'s words after the stamp, between an odd and an even
  // stamp.
  void Store(const SessionHeaderRecord& record) noexcept;

  void* block_;
  DumpStreamHeader* header_;
  std::uint64_t* words_;
  // The writer's copy of the record.
  SessionHeaderRecord current_{};
};

}  // namespace se

#endif  // SE_CORE_SESSION_HEADER_H_
