// Copyright 2026 The simple-engine-2 Authors.
//
// Replays: the header, the start bytes (the scene, or the editor's
// Play-start bytes), every InputFrame, the per-tick hashes and the
// SessionLog, written as one BEVE object:
//
//   {"format": "se-replay", "version": 1,
//    "header": {"build", "platform", "compiler", "cpu_features",
//               "math_hash", "scene_hash", "seed", "tick_rate"},
//    "start": u8[], "first_tick": u64, "frame_count": u64, "frames": u8[],
//    "first_hash_tick": u64, "hashes": u64[], "session": u8[]}
//
// Frames are packed little-endian records: tick, held, pressed[64],
// released[64], axes[8] (float bits), platform_live, edge_count, then each
// edge (action, down, stamp_ns, order, source). The header's math hash is
// dm::SelfTestHash() of the recording build; a different value at replay
// time means the maths differ and hashes may too.

#ifndef SE_SIM_REPLAY_H_
#define SE_SIM_REPLAY_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/types.h"
#include "se_sim/session_log.h"

namespace se {

struct ReplayHeader {
  std::string build;
  std::string platform;
  std::string compiler;
  std::uint64_t cpu_features = 0;
  std::uint64_t math_hash = 0;
  std::uint64_t scene_hash = 0;
  std::uint64_t seed = 0;
  std::uint32_t tick_rate = 120;

  friend bool operator==(const ReplayHeader&, const ReplayHeader&) = default;
};

// A header for this build: platform, compiler, CPU features, the math
// self-test hash and the hash of |start_bytes|.
[[nodiscard]] ReplayHeader MakeReplayHeader(
    std::span<const std::byte> start_bytes, std::uint64_t seed,
    std::uint32_t tick_rate);

class ReplayRecorder {
 public:
  void Begin(const ReplayHeader& header, std::span<const std::byte> start);
  // Replaces the start bytes and the header's hash of them (M13: a
  // multi-state session's content bundle, set at shutdown); ignored when
  // not active.
  void SetStart(std::span<const std::byte> start);
  [[nodiscard]] const ReplayHeader& Header() const noexcept { return header_; }
  [[nodiscard]] bool Active() const noexcept { return active_; }
  // Frames must arrive in consecutive ticks; ignored when not active.
  void Record(const InputFrame& frame);
  void RecordHash(Tick tick, std::uint64_t hash);
  // Stores the log's entries (call once, at the end).
  void RecordSession(const SessionLog& log);
  // The replay file; the recorder becomes inactive.
  [[nodiscard]] std::vector<std::byte> Finish();

  [[nodiscard]] std::size_t FrameCount() const noexcept { return frames_; }

 private:
  bool active_ = false;
  ReplayHeader header_;
  std::vector<std::byte> start_;
  Tick first_tick_ = 0;
  std::size_t frames_ = 0;
  std::vector<std::byte> frame_bytes_;
  Tick first_hash_tick_ = 0;
  std::vector<std::uint64_t> hashes_;
  std::vector<std::byte> session_;
};

// Plays a replay's frames back as an InputSource.
class ReplayReader final : public InputSource {
 public:
  [[nodiscard]] static std::expected<ReplayReader, Error> Open(
      std::span<const std::byte> bytes);

  // The recorded frame for |tick|; an empty frame past the end.
  InputFrame Latch(Tick tick, std::uint64_t tick_window_end_ns,
                   bool last_tick_of_frame) override;

  [[nodiscard]] const ReplayHeader& Header() const noexcept { return header_; }
  [[nodiscard]] std::span<const std::byte> StartBytes() const noexcept {
    return start_;
  }
  [[nodiscard]] Tick FirstTick() const noexcept { return first_tick_; }
  [[nodiscard]] std::span<const InputFrame> Frames() const noexcept {
    return frames_;
  }
  [[nodiscard]] Tick FirstHashTick() const noexcept { return first_hash_tick_; }
  [[nodiscard]] std::span<const std::uint64_t> Hashes() const noexcept {
    return hashes_;
  }
  [[nodiscard]] std::span<const std::byte> SessionBytes() const noexcept {
    return session_;
  }
  // The recording build's maths match this build's (dm::SelfTestHash).
  [[nodiscard]] bool MathMatches() const noexcept;

 private:
  ReplayHeader header_;
  std::vector<std::byte> start_;
  Tick first_tick_ = 0;
  std::vector<InputFrame> frames_;
  Tick first_hash_tick_ = 0;
  std::vector<std::uint64_t> hashes_;
  std::vector<std::byte> session_;
};

}  // namespace se

#endif  // SE_SIM_REPLAY_H_
