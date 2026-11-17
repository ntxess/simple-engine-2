// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor_panels/gizmo.h"

#include <cmath>

#include "se_core/types.h"

namespace se::editor {

GizmoAxis HitGizmo(Vec2 origin, Vec2 mouse) noexcept {
  const Vec2 d = mouse - origin;
  if (std::fabs(d.x) <= kGizmoCenterHalf &&
      std::fabs(d.y) <= kGizmoCenterHalf) {
    return GizmoAxis::kBoth;
  }
  if (d.x >= 0.0F && d.x <= kGizmoLength &&
      std::fabs(d.y) <= kGizmoPickRadius) {
    return GizmoAxis::kX;
  }
  if (d.y >= 0.0F && d.y <= kGizmoLength &&
      std::fabs(d.x) <= kGizmoPickRadius) {
    return GizmoAxis::kY;
  }
  return GizmoAxis::kNone;
}

Vec2 GizmoDragPosition(GizmoAxis axis, Vec2 start_pos, Vec2 start_mouse,
                       Vec2 mouse, float scale) noexcept {
  if (axis == GizmoAxis::kNone || !(scale > 0.0F)) return start_pos;
  const Vec2 travel = (mouse - start_mouse) * (1.0F / scale);
  Vec2 pos = start_pos;
  if (axis != GizmoAxis::kY) pos.x += travel.x;
  if (axis != GizmoAxis::kX) pos.y += travel.y;
  return pos;
}

}  // namespace se::editor
