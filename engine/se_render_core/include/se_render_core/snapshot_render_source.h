// Copyright 2026 The simple-engine-2 Authors.
//
// The platform side of the dedicated sim driver's snapshots (docs/
// milestone-specs.md M17a; docs/design/dedicated-driver.md "Snapshot And
// Interpolation", PAIRING AND INTERPOLATION; ADR-086).
//
// SnapshotRenderSource, once per frame on the platform thread:
//   Receive  pops every published slot (SnapshotChannel::Receive), hands
//            each one's reliable section to the caller in tick order
//            (whether or not the slot is ever drawn), returns reliable-only
//            slots at once, keeps the newest 3 render slots and returns the
//            rest; the slots of a session that ended or faulted are
//            returned as soon as its end arrives.
//   Pick     tau = F - D' for the frame start F; the pair S_a, S_b with
//            anchor_a <= tau < anchor_b, alpha = (tau - anchor_a) /
//            (anchor_b - anchor_a) from the real anchor spacing (slow
//            motion and dilation need no special case).
//              starvation  tau past the newest anchor while the session
//                          runs: the newest pair at alpha 1 (never
//                          extrapolated), and D' rises by the gap; after
//                          that the rise decays by at most 5% per frame, so
//                          motion resumes without a jump. A paused or
//                          suspended session holds its newest pair at
//                          alpha 1 without raising D';
//              early       tau before the oldest anchor (the first frames):
//                          the oldest pair at alpha 0;
//              hard cut    S_a and S_b differ in session or generation: S_b
//                          alone, and the older generation's slots go back.
//   Build    the pair's RenderList for one view (BuildSnapshotRenderList).
//
// BuildSnapshotRenderList(a, b, view, alpha) interpolates per entity of
// S_b, looking it up in S_a's sparse index:
//   - same full id and spawn serial, no teleport flag: position and scale
//     linearly, rotation along the shorter arc, tint linearly; the discrete
//     state (texture, region, origin, flips, layer, z, visibility) from S_a
//     below alpha 0.5, from S_b from 0.5 on;
//   - same entity, teleport flag: S_a's state below the half-tick, S_b's
//     from it (no smear);
//   - only in S_b (a spawn, or a recycled index with a new spawn serial):
//     appears at the half-tick; only in S_a (a despawn, or the index's
//     previous occupant): disappears at the half-tick;
//   - no S_a (the first slot, a hard cut): S_b's state.
// The view's camera is interpolated the same way (centre and zoom) unless
// S_b's camera has the cut flag; a view whose World did not step is drawn
// at alpha 1. The quads come from the same code as BuildRenderList's
// (src/quad_builder.h): for slots extracted from a World's ticks k - 1 and k
// (AppendRenderView), the list equals BuildRenderList(World at k, camera,
// alpha) for every alpha whenever the state matches (the same entities, no
// teleports, unchanged discrete state and tint, a camera that did not
// move), and at alpha 1 always.
//
// The source reads no clock: the frame start comes from the caller (the
// FramePacer's), so tests drive it with any timeline. Platform thread only;
// Pick and Build read the retained slots, so Build follows Pick before the
// next Receive.

#ifndef SE_RENDER_CORE_SNAPSHOT_RENDER_SOURCE_H_
#define SE_RENDER_CORE_SNAPSHOT_RENDER_SOURCE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "se_core/snapshot.h"
#include "se_core/snapshot_channel.h"
#include "se_render_core/render_list.h"

namespace se {

// Builds view |view| of |newer| into |list|, interpolated from |older| (null:
// |newer| alone) by |alpha| (clamped to [0, 1]; NaN draws |newer|). An
// |older| slot of another session or generation is ignored (a hard cut).
void BuildSnapshotRenderList(const Snapshot* older, const Snapshot& newer,
                             std::uint32_t view, float alpha, RenderList& list);

struct SnapshotRenderSourceConfig {
  // D' when nothing starves: the platform's c_p95 + m (Free cadence, about
  // 4 ms), or c_p95 + m - dt when the sim's phase is locked to the display
  // (negative: tau is then ahead of the frame start). SetDelay changes it.
  std::int64_t delay_ns = 4'000'000;
  // After a starvation raised D', the raise shrinks by at most this
  // fraction per frame.
  double max_decay_per_frame = 0.05;
};

struct SnapshotRenderStats {
  std::uint64_t received = 0;
  // Slots without a render part (returned at once).
  std::uint64_t reliable_only = 0;
  std::uint64_t returned = 0;
  std::uint64_t frames = 0;
  // Frames whose tau was past the newest anchor while the session ran.
  std::uint64_t starved_frames = 0;
  // Frames whose tau was before the oldest anchor.
  std::uint64_t early_frames = 0;
  // Frames that held the newest pair of a paused or suspended session.
  std::uint64_t held_frames = 0;
  std::uint64_t hard_cuts = 0;
  // The newest slot header's count of the sim's skipped publishes.
  std::uint64_t dropped_publishes = 0;
};

// What Pick chose for a frame.
struct SnapshotPick {
  // Null: |b| alone.
  const Snapshot* a = nullptr;
  // Null: nothing to draw.
  const Snapshot* b = nullptr;
  float alpha = 1.0F;
  std::int64_t tau_ns = 0;
  bool starved = false;
  bool early = false;
  bool hard_cut = false;
};

class SnapshotRenderSource {
 public:
  // Render slots kept between frames.
  static constexpr std::size_t kRetained = 3;

  explicit SnapshotRenderSource(SnapshotChannel& channel,
                                SnapshotRenderSourceConfig config = {});
  SnapshotRenderSource(const SnapshotRenderSource&) = delete;
  SnapshotRenderSource& operator=(const SnapshotRenderSource&) = delete;
  // Returns every retained slot.
  ~SnapshotRenderSource() = default;

  // Takes every published slot, calling on_reliable(header, deltas) for
  // each one with a non-empty reliable section, in tick order; returns how
  // many arrived.
  template <class F>
    requires std::is_invocable_v<F&, const SnapshotHeader&,
                                 const ReliableDeltas&>
  std::uint32_t Receive(F&& on_reliable) {
    auto&& visit = std::forward<F>(on_reliable);
    return channel_->Receive([this, &visit](SlotLease lease) {
      if (!lease->reliable.Empty()) visit(lease->header, lease->reliable);
      Keep(std::move(lease));
    });
  }
  // The same, ignoring the reliable sections.
  std::uint32_t Receive() {
    return Receive([](const SnapshotHeader&, const ReliableDeltas&) {});
  }
  // Chooses the pair for the frame that starts at |frame_start_ns|.
  SnapshotPick Pick(std::uint64_t frame_start_ns);
  // The last pick's RenderList of view |view| (empty when it has nothing
  // to draw or no such view).
  void Build(std::uint32_t view, RenderList& list) const;
  // Views of the last pick's S_b.
  [[nodiscard]] std::uint32_t ViewCount() const noexcept;
  [[nodiscard]] const SnapshotPick& LastPick() const noexcept { return pick_; }

  // The base delay (see SnapshotRenderSourceConfig::delay_ns).
  void SetDelay(std::int64_t delay_ns) noexcept { config_.delay_ns = delay_ns; }
  // D' now: the base delay plus what starvation added.
  [[nodiscard]] std::int64_t DelayNs() const noexcept {
    return config_.delay_ns + raise_ns_;
  }
  [[nodiscard]] std::size_t Retained() const noexcept { return count_; }
  // The retained slot |i| (0 the oldest).
  [[nodiscard]] const Snapshot& RetainedSlot(std::size_t i) const noexcept {
    return *retained_[i];
  }
  // Returns every retained slot (shutdown, or the platform stops drawing).
  void ReleaseAll() noexcept;
  [[nodiscard]] const SnapshotRenderStats& Stats() const noexcept {
    return stats_;
  }

 private:
  // Retains a received slot (or returns it: reliable-only, an ended
  // session's end).
  void Keep(SlotLease lease) noexcept;
  // Returns the oldest |n| retained slots.
  void ReleaseOldest(std::size_t n) noexcept;
  void ReleaseSession(std::uint64_t session) noexcept;

  SnapshotChannel* channel_;
  SnapshotRenderSourceConfig config_;
  // Oldest first; one spare entry for the slot being added.
  std::array<SlotLease, kRetained + 1> retained_;
  std::size_t count_ = 0;
  std::int64_t raise_ns_ = 0;
  SnapshotPick pick_;
  SnapshotRenderStats stats_;
};

}  // namespace se

#endif  // SE_RENDER_CORE_SNAPSHOT_RENDER_SOURCE_H_
