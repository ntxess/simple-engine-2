// Copyright 2026 The simple-engine-2 Authors.
//
// The sim side of a snapshot slot's render part (docs/milestone-specs.md
// M17a; docs/design/dedicated-driver.md "Snapshot And Interpolation",
// CONTENTS and BUILD; ADR-086). After a tick's Cleanup, when deferred
// destroys have drained and the spawns are finalized, the sim thread's
// session (SimSession::FillRender) calls AppendRenderView once per drawn
// World, bottom of the GameStateStack first. One pass over the World's
// SpriteRenderers writes, into the slot's reused vectors:
//   - a RenderItem for every entity with a SpriteRenderer, a Transform and a
//     Spawned record (BuildRenderList's sprites): the full EnTT id, the
//     spawn serial, the Transform's pose and the SpriteRenderer's look, with
//     kItemVisible and the flip bits;
//   - kItemTeleported where BuildRenderList would not interpolate the
//     entity at this tick (teleport_tick is the tick, no PrevTransform, or
//     one not from the tick before), and where it moved more than
//     teleport_distance since the tick before (the design's displacement
//     rule, 64 px per tick by default);
//   - the sparse index (entity index to item), sized to the largest live
//     index;
//   - the view: the camera the World is drawn through, its cut flag and
//     whether the World stepped this tick.
// Nothing is culled or sorted here: SnapshotRenderSource builds the
// RenderList on the platform side, through the frame's camera. A warm slot
// allocates nothing.

#ifndef SE_RENDER_CORE_SNAPSHOT_FILL_H_
#define SE_RENDER_CORE_SNAPSHOT_FILL_H_

#include <cstdint>

#include "se_core/snapshot.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"

namespace se {

struct RenderViewOptions {
  // The camera the World is drawn through (the App's CameraFor: the World's
  // primary Camera2D, or the game's own).
  Camera2D camera;
  // The camera jumped this tick: the platform never interpolates into it.
  bool camera_cut = false;
  // The World stepped this tick (kViewStepped). A World that did not (a
  // state below the top) is drawn as it is, at alpha 1.
  bool stepped = true;
  // A move longer than this since the tick before (world pixels) is a
  // teleport; 0 turns the rule off.
  float teleport_distance = 64.0F;
};

// Appends a view of |world| to |slot| (after the views already in it).
// False, with the slot unchanged, when the slot already has
// kMaxSnapshotViews views.
bool AppendRenderView(const World& world, const RenderViewOptions& options,
                      Snapshot& slot);

// The entity index of a RenderItem's full id (EnTT's index bits): where the
// sparse index keeps it.
[[nodiscard]] std::uint32_t SnapshotEntityIndex(std::uint32_t entity) noexcept;

}  // namespace se

#endif  // SE_RENDER_CORE_SNAPSHOT_FILL_H_
