// Copyright 2026 The simple-engine-2 Authors.
//
// The Viewport's 2D move gizmo as pure geometry (docs/design/mt-design.md
// "Editor": gizmos are ImGui overlays; a drag previews and commits one
// coalesced command). Drawn at the selected entity's position on screen: an
// X handle (red, to the right), a Y handle (green, downwards: y grows down)
// and a square in the middle that moves on both axes. A press on a handle
// starts a drag; the entity's new position is its position at the press
// plus the mouse's travel in world pixels, along the handle's axis.

#ifndef SE_EDITOR_PANELS_GIZMO_H_
#define SE_EDITOR_PANELS_GIZMO_H_

#include <cstdint>

#include "se_core/types.h"

namespace se::editor {

inline constexpr float kGizmoLength = 60.0F;
// How far from a handle's line a press still takes it, in screen pixels.
inline constexpr float kGizmoPickRadius = 6.0F;
// Half the side of the middle square.
inline constexpr float kGizmoCenterHalf = 7.0F;

enum class GizmoAxis : std::uint8_t { kNone, kX, kY, kBoth };

// The handle under |mouse| for a gizmo drawn at |origin| (screen pixels).
[[nodiscard]] GizmoAxis HitGizmo(Vec2 origin, Vec2 mouse) noexcept;

// The position during a drag: |start_pos| (world) moved by the mouse's
// travel from |start_mouse| to |mouse| (screen) at |scale| screen pixels
// per world pixel, along |axis|.
[[nodiscard]] Vec2 GizmoDragPosition(GizmoAxis axis, Vec2 start_pos,
                                     Vec2 start_mouse, Vec2 mouse,
                                     float scale) noexcept;

}  // namespace se::editor

#endif  // SE_EDITOR_PANELS_GIZMO_H_
