// Copyright 2026 The simple-engine-2 Authors.
//
// A snapshot slot: the complete presentation state of one tick of one
// session, written by the sim thread and read, immutable, by the platform
// (docs/design/dedicated-driver.md "Snapshot And Interpolation"; ADR-085).
//
//   header    session, generation, anchor epoch, tick, wall anchor (L_k + dt,
//             the wall time the state represents, since the sim runs ahead),
//             latch time L_k, the last applied command sequence, tick cost,
//             dropped publishes, the hash after the tick, the run state
//   items     one trivially copyable RenderItem per drawn sprite: keys and
//             ids only, never pointers
//   sparse    per view, entity index -> item, sized to the largest live index
//   views     one per drawn World (bottom first): its camera and its ranges
//             of items and sparse entries
//   reliable  deltas that must never be lost (audio cues, session events,
//             scope releases, tick records, an ended session's replay),
//             accumulated since the last successful publish
//
// Everything is plain data in reused vectors, so a warm slot allocates
// nothing. SnapshotChannel (se_core/snapshot_channel.h) moves slots between
// the threads; se_render_core fills and reads them.

#ifndef SE_CORE_SNAPSHOT_H_
#define SE_CORE_SNAPSHOT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "se_core/types.h"

namespace se {

// RenderItem::flags.
inline constexpr std::uint8_t kItemTeleported = 1U << 0;
inline constexpr std::uint8_t kItemVisible = 1U << 1;
inline constexpr std::uint8_t kItemFlipX = 1U << 2;
inline constexpr std::uint8_t kItemFlipY = 1U << 3;

// A sparse entry with no item.
inline constexpr std::uint32_t kNoSnapshotItem = 0xFFFFFFFFU;
// Views (drawn Worlds) one slot can carry.
inline constexpr std::uint32_t kMaxSnapshotViews = 4;

// One sprite as the tick left it. 68 bytes: the region and origin are the
// SpriteRenderer's floats (an atlas region index arrives with M9's atlases).
struct RenderItem {
  // The full EnTT id (index and version) and the World's spawn serial; a
  // recycled index is never paired with its previous occupant.
  std::uint32_t entity = 0;
  std::uint32_t spawn_serial = 0;
  Vec2 pos;
  float rot = 0.0F;
  Vec2 scale{1.0F, 1.0F};
  // SpriteRenderer's texture key.
  std::uint32_t texture = 0;
  Rect region;
  Vec2 origin;
  Rgba8 tint;
  std::int16_t layer = 0;
  // kItem* bits. kItemTeleported: do not interpolate into this state.
  std::uint8_t flags = 0;
  std::uint8_t reserved = 0;
  float z = 0.0F;

  [[nodiscard]] constexpr bool Has(std::uint8_t bit) const noexcept {
    return (flags & bit) != 0;
  }
};

static_assert(std::is_trivially_copyable_v<RenderItem>);
static_assert(sizeof(RenderItem) == 68, "RenderItem layout changed");

// A view's camera. Camera2D's fields plus a rotation (not drawn yet) and the
// cut flag: a cut camera is never interpolated into.
struct CameraState {
  Vec2 center;
  float zoom = 1.0F;
  float rotation = 0.0F;
  Vec2 virtual_size{1920.0F, 1080.0F};
  bool integer_scale = false;
  bool primary = false;
  bool cut = false;
};

// SnapshotView::flags.
// The View's World stepped this tick; others are drawn as they are (alpha 1),
// as GameStateStack::RenderSources draws the states below the top.
inline constexpr std::uint8_t kViewStepped = 1U << 0;

struct SnapshotView {
  CameraState camera;
  std::uint32_t first_item = 0;
  std::uint32_t item_count = 0;
  std::uint32_t first_sparse = 0;
  std::uint32_t sparse_count = 0;
  std::uint8_t flags = 0;
};

enum class SimRunState : std::uint8_t {
  // Ticking on its deadlines.
  kRunning,
  // Stopped at a tick boundary (Pause); Step{n} ticks publish in this state.
  kPaused,
  // Minimized: no ticks (Suspend).
  kSuspended,
};

struct SnapshotHeader {
  std::uint64_t session = 0;
  // Bumped when the session starts and whenever its GameStateStack changes
  // (scene load, GameState switch): slots of different generations are never
  // interpolated.
  std::uint32_t generation = 0;
  // Bumped whenever the sim re-bases its clock (resume, dilation, time scale,
  // phase): anchors of different epochs are not on one grid.
  std::uint32_t epoch = 0;
  Tick tick = 0;
  // L_k + dt (wall time the state represents) and L_k (input latched up to),
  // on the engine's monotonic clock.
  std::uint64_t anchor_ns = 0;
  std::uint64_t latch_ns = 0;
  // The sequence number of the last SimCommand applied before this tick.
  std::uint32_t acked_seq = 0;
  // Publishes the session skipped for want of a free slot, so far.
  std::uint32_t dropped_publishes = 0;
  // CPU time of the tick (RunTick), and of building this slot.
  std::uint64_t tick_cost_ns = 0;
  std::uint64_t build_cost_ns = 0;
  // GameStateStack::Hash().Folded() after the tick.
  std::uint64_t world_hash = 0;
  SimRunState state = SimRunState::kRunning;
  // False for a slot that carries only reliable deltas (a session's end):
  // the platform applies them and returns the slot at once.
  bool render = true;
};

// A sound a tick asked for; the platform schedules it at the tick's anchor
// plus its audio delay (M8 owns the voices).
struct AudioCue {
  Tick tick = 0;
  std::uint64_t sound = 0;
  Vec2 pos;
  float gain = 1.0F;
  float pitch = 1.0F;
  std::uint8_t policy = 0;
};

enum class SessionEventKind : std::uint8_t {
  kStarted,
  // StartGame's factory returned an error (value: the ErrorCode).
  kStartFailed,
  kGenerationChanged,
  kPaused,
  kResumed,
  kSuspended,
  kStepped,
  kDilated,
  kFaulted,
  kEnded,
};

struct SessionEvent {
  SessionEventKind kind = SessionEventKind::kStarted;
  std::uint64_t session = 0;
  std::uint32_t generation = 0;
  // The tick the event happened before (the session's next tick).
  Tick tick = 0;
  // Kind-specific: ticks stepped, nanoseconds dilated.
  std::uint64_t value = 0;
};

// GPU objects of |scope| may be freed once every slot with a tick below
// |release_tick| is returned and the last frame that drew them has finished.
struct ScopeRelease {
  std::uint64_t scope = 0;
  Tick release_tick = 0;
};

// The result of one tick (SimDriver's TickResult without the state name).
struct TickRecord {
  std::uint64_t session = 0;
  Tick tick = 0;
  std::uint64_t hash = 0;
  std::uint32_t worlds_stepped = 0;
};

struct ReliableDeltas {
  std::vector<AudioCue> audio;
  std::vector<SessionEvent> events;
  std::vector<ScopeRelease> releases;
  std::vector<TickRecord> ticks;
  // The finished replay of a session that ended while recording (the
  // design's "records the replay tail"), for the platform to keep.
  std::vector<std::byte> replay;

  [[nodiscard]] bool Empty() const noexcept {
    return audio.empty() && events.empty() && releases.empty() &&
           ticks.empty() && replay.empty();
  }
  // Keeps the capacity.
  void Clear() noexcept {
    audio.clear();
    events.clear();
    releases.clear();
    ticks.clear();
    replay.clear();
  }
};

struct Snapshot {
  SnapshotHeader header;
  std::vector<RenderItem> items;
  std::vector<std::uint32_t> sparse;
  std::array<SnapshotView, kMaxSnapshotViews> views{};
  std::uint32_t view_count = 0;
  ReliableDeltas reliable;

  // Empties the render part (keeps the capacity); the reliable section is
  // the writer's to manage.
  void ClearRender() noexcept {
    items.clear();
    sparse.clear();
    view_count = 0;
  }

  [[nodiscard]] std::span<const SnapshotView> Views() const noexcept {
    return std::span<const SnapshotView>(views.data(), view_count);
  }
  [[nodiscard]] std::span<const RenderItem> Items(
      const SnapshotView& view) const noexcept {
    return std::span<const RenderItem>(items).subspan(view.first_item,
                                                      view.item_count);
  }
  // The item of the entity with index |entity_index| in |view|, or nullptr.
  [[nodiscard]] const RenderItem* Find(
      const SnapshotView& view, std::uint32_t entity_index) const noexcept {
    if (entity_index >= view.sparse_count) return nullptr;
    const std::uint32_t at = sparse[view.first_sparse + entity_index];
    return at == kNoSnapshotItem ? nullptr : &items[at];
  }
};

}  // namespace se

#endif  // SE_CORE_SNAPSHOT_H_
