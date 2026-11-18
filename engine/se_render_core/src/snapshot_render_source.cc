// Copyright 2026 The simple-engine-2 Authors.

#include "se_render_core/snapshot_render_source.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "se_core/snapshot.h"
#include "se_core/snapshot_channel.h"
#include "se_core/types.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"
#include "se_render_core/snapshot_fill.h"
#include "src/quad_builder.h"

namespace se {
namespace {

using render_internal::Canvas;
using render_internal::CanvasOf;
using render_internal::ClampAlpha;
using render_internal::EmitQuad;
using render_internal::FinishList;
using render_internal::KeyBits;
using render_internal::Mark;
using render_internal::Pose;
using render_internal::PrepareList;
using render_internal::ShortestArc;
using render_internal::SortItem;

// The quads of one snapshot build: EmitQuad around counters kept here.
struct Emitter {
  Emitter(RenderList& list, const Camera2D& camera, std::size_t capacity)
      : canvas(CanvasOf(camera)),
        out(PrepareList(list, capacity).quads.data()),
        items(list.scratch.items.data()) {}
  // A sprite at |pose| that looks like |look|, tinted |tint|.
  void Add(const Pose& pose, const RenderItem& look, Rgba8 tint) noexcept {
    ++candidates;
    const Mark mark{tint, look.Has(kItemFlipX), look.Has(kItemFlipY),
                    look.spawn_serial};
    if (EmitQuad(pose, look, mark, canvas, visible, out[visible],
                 items[visible])) {
      bits.Add(items[visible]);
      ++visible;
    }
  }
  // |item| as it is.
  void Add(const RenderItem& item) noexcept {
    Add(Pose{item.pos, item.rot, item.scale}, item, item.tint);
  }

  Canvas canvas;
  SpriteQuad* out = nullptr;
  SortItem* items = nullptr;
  KeyBits bits;
  std::uint32_t visible = 0;
  std::uint32_t candidates = 0;
};

// A raise of D' below this is gone.
constexpr std::int64_t kRaiseFloorNs = 1'000;

std::uint8_t LerpChannel(std::uint8_t from, std::uint8_t to, float a) {
  const float value = static_cast<float>(from) +
                      (static_cast<float>(to) - static_cast<float>(from)) * a;
  return static_cast<std::uint8_t>(std::lround(value));
}

// Exact at both ends and wherever the two tints are equal.
Rgba8 LerpTint(const Rgba8& from, const Rgba8& to, float a) {
  if (from == to || a >= 1.0F) return to;
  return Rgba8{LerpChannel(from.r, to.r, a), LerpChannel(from.g, to.g, a),
               LerpChannel(from.b, to.b, a), LerpChannel(from.a, to.a, a)};
}

bool SameEntity(const RenderItem& a, const RenderItem& b) {
  return a.entity == b.entity && a.spawn_serial == b.spawn_serial;
}

// The entity of |item| in |view| of |slot|, if the same one is there.
const RenderItem* Match(const Snapshot& slot, const SnapshotView& view,
                        const RenderItem& item) {
  const RenderItem* found = slot.Find(view, SnapshotEntityIndex(item.entity));
  return found != nullptr && SameEntity(*found, item) ? found : nullptr;
}

void ClearList(RenderList& list) {
  list.stats = RenderStats{};
  list.quads.clear();
  list.batches.clear();
}

}  // namespace

void BuildSnapshotRenderList(const Snapshot* older, const Snapshot& newer,
                             std::uint32_t view, float alpha,
                             RenderList& list) {
  if (view >= newer.view_count) {
    ClearList(list);
    return;
  }
  const SnapshotView& view_b = newer.views[view];
  const SnapshotView* view_a = nullptr;
  if (older != nullptr && older != &newer &&
      older->header.session == newer.header.session &&
      older->header.generation == newer.header.generation &&
      view < older->view_count) {
    view_a = &older->views[view];
  }
  // A World that did not step is drawn as it is.
  const float a = (view_b.flags & kViewStepped) != 0 ? ClampAlpha(alpha) : 1.0F;
  const float b = 1.0F - a;
  const bool first_half = a < 0.5F;

  Camera2D camera;
  camera.center = view_b.camera.center;
  camera.zoom = view_b.camera.zoom;
  camera.virtual_size = view_b.camera.virtual_size;
  camera.integer_scale = view_b.camera.integer_scale;
  camera.primary = view_b.camera.primary;
  if (view_a != nullptr && !view_b.camera.cut) {
    const CameraState& from = view_a->camera;
    if (from.center != camera.center) {
      camera.center = camera.center * a + from.center * b;
    }
    if (from.zoom != camera.zoom) camera.zoom = camera.zoom * a + from.zoom * b;
  }
  if (!(camera.zoom > 0.0F)) {
    ClearList(list);
    return;
  }

  const std::size_t capacity =
      std::size_t{view_b.item_count} +
      (view_a != nullptr ? std::size_t{view_a->item_count} : 0);
  Emitter builder(list, camera, capacity);
  std::uint32_t interpolated = 0;
  for (const RenderItem& item_b : newer.Items(view_b)) {
    const RenderItem* item_a =
        view_a != nullptr ? Match(*older, *view_a, item_b) : nullptr;
    if (view_a != nullptr && item_a == nullptr) {
      // Spawned since S_a, or a new occupant of a recycled index: it
      // appears at the half-tick.
      if (first_half || !item_b.Has(kItemVisible)) continue;
      builder.Add(item_b);
      continue;
    }
    if (item_a == nullptr || item_b.Has(kItemTeleported)) {
      // No S_a, or a teleport: one state, switching at the half-tick.
      const RenderItem& state =
          item_a != nullptr && first_half ? *item_a : item_b;
      if (!state.Has(kItemVisible)) continue;
      builder.Add(state);
      continue;
    }
    // The discrete state from the nearer slot, the pose in between. Exact
    // at both ends, and the same arithmetic as BuildRenderList's.
    const RenderItem& look = first_half ? *item_a : item_b;
    if (!look.Has(kItemVisible)) continue;
    Pose pose;
    pose.pos = item_b.pos * a + item_a->pos * b;
    pose.scale = item_b.scale * a + item_a->scale * b;
    pose.rot = item_b.rot - ShortestArc(item_a->rot, item_b.rot) * b;
    ++interpolated;
    builder.Add(pose, look, LerpTint(item_a->tint, item_b.tint, a));
  }
  if (view_a != nullptr && first_half) {
    // Despawned since S_a, or the previous occupant of a recycled index:
    // drawn until the half-tick.
    for (const RenderItem& item_a : older->Items(*view_a)) {
      if (Match(newer, view_b, item_a) != nullptr) continue;
      if (!item_a.Has(kItemVisible)) continue;
      builder.Add(item_a);
    }
  }
  FinishList(list, builder.visible, builder.bits, builder.candidates,
             interpolated);
}

SnapshotRenderSource::SnapshotRenderSource(SnapshotChannel& channel,
                                           SnapshotRenderSourceConfig config)
    : channel_(&channel), config_(config) {}

void SnapshotRenderSource::Keep(SlotLease lease) noexcept {
  ++stats_.received;
  const Snapshot& slot = *lease;
  stats_.dropped_publishes = slot.header.dropped_publishes;
  // A session that ended or faulted is not drawn any more.
  for (const SessionEvent& event : slot.reliable.events) {
    if (event.kind == SessionEventKind::kEnded ||
        event.kind == SessionEventKind::kFaulted) {
      ReleaseSession(event.session);
    }
  }
  if (!slot.header.render) {
    // Nothing to draw: its reliable section was all it carried.
    ++stats_.reliable_only;
    ++stats_.returned;
    return;
  }
  retained_[count_++] = std::move(lease);
  if (count_ > kRetained) ReleaseOldest(count_ - kRetained);
}

void SnapshotRenderSource::ReleaseOldest(std::size_t n) noexcept {
  if (n == 0) return;
  // The pick may point at a returned slot.
  pick_ = SnapshotPick{};
  for (std::size_t i = 0; i < n; ++i) {
    retained_[i].Reset();
    ++stats_.returned;
  }
  for (std::size_t i = n; i < count_; ++i) {
    retained_[i - n] = std::move(retained_[i]);
  }
  count_ -= n;
}

void SnapshotRenderSource::ReleaseSession(std::uint64_t session) noexcept {
  std::size_t kept = 0;
  for (std::size_t i = 0; i < count_; ++i) {
    if (retained_[i]->header.session == session) {
      pick_ = SnapshotPick{};
      retained_[i].Reset();
      ++stats_.returned;
      continue;
    }
    if (kept != i) retained_[kept] = std::move(retained_[i]);
    ++kept;
  }
  count_ = kept;
}

void SnapshotRenderSource::ReleaseAll() noexcept { ReleaseOldest(count_); }

SnapshotPick SnapshotRenderSource::Pick(std::uint64_t frame_start_ns) {
  ++stats_.frames;
  SnapshotPick pick;
  pick.tau_ns = static_cast<std::int64_t>(frame_start_ns) - DelayNs();
  const auto anchor = [this](std::size_t i) {
    return static_cast<std::int64_t>(retained_[i]->header.anchor_ns);
  };
  std::int64_t gap = 0;
  if (count_ == 1) {
    pick.b = retained_[0].Get();
  } else if (count_ >= 2) {
    const std::size_t newest = count_ - 1;
    std::size_t at_b = newest;
    if (pick.tau_ns >= anchor(newest)) {
      // Nothing newer to move towards: hold the newest pair at alpha 1.
      pick.alpha = 1.0F;
      gap = pick.tau_ns - anchor(newest);
    } else if (pick.tau_ns < anchor(0)) {
      at_b = 1;
      pick.alpha = 0.0F;
      pick.early = true;
      ++stats_.early_frames;
    } else {
      // anchor(at_b - 1) <= tau < anchor(at_b).
      at_b = 1;
      while (pick.tau_ns >= anchor(at_b)) ++at_b;
      const std::int64_t span = anchor(at_b) - anchor(at_b - 1);
      const double into = static_cast<double>(pick.tau_ns - anchor(at_b - 1));
      pick.alpha = span > 0
                       ? static_cast<float>(into / static_cast<double>(span))
                       : 1.0F;
    }
    const SnapshotHeader& header_a = retained_[at_b - 1]->header;
    const SnapshotHeader& header_b = retained_[at_b]->header;
    if (header_a.session != header_b.session ||
        header_a.generation != header_b.generation) {
      // A new scene or session: no blending across it, and the older
      // generation's slots go back. Its first frames are no starvation.
      ++stats_.hard_cuts;
      ReleaseOldest(at_b);
      pick.hard_cut = true;
      pick.alpha = 1.0F;
      pick.b = retained_[0].Get();
      gap = 0;
    } else {
      pick.a = retained_[at_b - 1].Get();
      pick.b = retained_[at_b].Get();
    }
  }
  if (gap > 0 && pick.b->header.state != SimRunState::kRunning) {
    // Paused or suspended: no newer slot is coming.
    ++stats_.held_frames;
  } else if (gap > 0) {
    // Starved: D' rises to cover the gap, holding tau at the newest
    // anchor.
    pick.starved = true;
    pick.tau_ns -= gap;
    raise_ns_ += gap;
    ++stats_.starved_frames;
  } else if (raise_ns_ > 0) {
    raise_ns_ = static_cast<std::int64_t>(static_cast<double>(raise_ns_) *
                                          (1.0 - config_.max_decay_per_frame));
    if (raise_ns_ < kRaiseFloorNs) raise_ns_ = 0;
  }
  pick_ = pick;
  return pick_;
}

void SnapshotRenderSource::Build(std::uint32_t view, RenderList& list) const {
  if (pick_.b == nullptr) {
    ClearList(list);
    return;
  }
  BuildSnapshotRenderList(pick_.a, *pick_.b, view, pick_.alpha, list);
}

std::uint32_t SnapshotRenderSource::ViewCount() const noexcept {
  return pick_.b != nullptr ? pick_.b->view_count : 0;
}

}  // namespace se
