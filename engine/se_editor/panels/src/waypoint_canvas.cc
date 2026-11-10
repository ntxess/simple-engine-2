// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor_panels/waypoint_canvas.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "game_rampage/components.h"
#include "se_core/types.h"

namespace se::editor {

CanvasMapping MapCanvas(Vec2 top_left, Vec2 draw_scale,
                        const CanvasView& view) noexcept {
  return CanvasMapping{top_left + view.scrolling, draw_scale, view.zoom};
}

Vec2 CanvasToWorld(const CanvasMapping& mapping, Vec2 screen) noexcept {
  return {
      (screen.x - mapping.origin.x) / (mapping.zoom * mapping.draw_scale.x),
      (screen.y - mapping.origin.y) / (mapping.zoom * mapping.draw_scale.y)};
}

Vec2 WorldToCanvas(const CanvasMapping& mapping, Vec2 world) noexcept {
  return {mapping.origin.x + world.x * mapping.draw_scale.x * mapping.zoom,
          mapping.origin.y + world.y * mapping.draw_scale.y * mapping.zoom};
}

float ZoomAfterWheel(float zoom, float wheel) noexcept {
  return std::clamp(zoom + wheel * kCanvasZoomStep, kCanvasZoomMin,
                    kCanvasZoomMax);
}

CanvasFrame ProcessCanvasInput(const CanvasView& view,
                               const CanvasMapping& mapping,
                               const CanvasInput& input) noexcept {
  CanvasFrame frame;
  frame.view = view;
  // The legacy code took the cursor's canvas position before panning.
  const Vec2 cursor = CanvasToWorld(mapping, input.mouse);
  if (input.active && input.right_dragging) {
    frame.view.scrolling = frame.view.scrolling + input.mouse_delta;
  }
  if (input.right_released && input.right_drag_delta == Vec2{}) {
    frame.open_menu = input.hovered;
  }
  if (input.hovered && input.left_clicked) frame.add_point = cursor;
  if (input.hovered && input.wheel != 0.0F) {
    frame.view.zoom = ZoomAfterWheel(frame.view.zoom, input.wheel);
  }
  return frame;
}

void CanvasGridLines(float scrolling, float zoom, float extent,
                     std::vector<float>& out) {
  out.clear();
  const float step = kCanvasGridStep * zoom;
  if (!(step > 0.0F) || !std::isfinite(step)) return;
  for (float x = std::fmod(scrolling * zoom, step); x < extent; x += step) {
    out.push_back(x);
  }
}

namespace {

std::optional<Vec2> MoveToFirst(const rampage::Path& path, Vec2 entity_pos) {
  if (path.points.empty() || path.points.front() == entity_pos) {
    return std::nullopt;
  }
  return path.points.front();
}

}  // namespace

PathEdit AppendPointEdit(const rampage::Path& path, Vec2 entity_pos,
                         Vec2 point) {
  PathEdit edit;
  edit.kind = PathEditKind::kAppend;
  auto size = static_cast<std::uint32_t>(path.points.size());
  if (size == 0) {
    edit.ops.push_back({PathPointOp::Kind::kInsert, 0, entity_pos});
    size = 1;
  } else {
    edit.entity_pos = MoveToFirst(path, entity_pos);
  }
  edit.ops.push_back({PathPointOp::Kind::kInsert, size, point});
  return edit;
}

std::optional<PathEdit> RemoveLastPointEdit(const rampage::Path& path,
                                            Vec2 entity_pos) {
  if (path.points.empty()) return std::nullopt;
  PathEdit edit;
  edit.kind = PathEditKind::kRemoveLast;
  edit.entity_pos = MoveToFirst(path, entity_pos);
  const auto size = static_cast<std::uint32_t>(path.points.size());
  if (size >= 2) {
    edit.ops.push_back({PathPointOp::Kind::kErase, size - 1, Vec2{}});
  }
  return edit;
}

std::optional<PathEdit> ClearPathEdit(const rampage::Path& path,
                                      Vec2 entity_pos) {
  if (path.points.empty()) return std::nullopt;
  PathEdit edit;
  edit.kind = PathEditKind::kClear;
  const auto size = static_cast<std::uint32_t>(path.points.size());
  for (std::uint32_t i = size; i > 1; --i) {
    edit.ops.push_back({PathPointOp::Kind::kErase, i - 1, Vec2{}});
  }
  if (path.points.front() != entity_pos) {
    edit.ops.push_back({PathPointOp::Kind::kMove, 0, entity_pos});
  }
  edit.loop = true;
  return edit;
}

void ApplyPathEdit(const PathEdit& edit, rampage::Path& path,
                   Vec2& entity_pos) {
  auto& points = path.points;
  for (const PathPointOp& op : edit.ops) {
    switch (op.kind) {
      case PathPointOp::Kind::kInsert:
        if (op.index <= points.size()) {
          points.insert(points.begin() + static_cast<std::ptrdiff_t>(op.index),
                        op.pos);
        }
        break;
      case PathPointOp::Kind::kMove:
        if (op.index < points.size()) points[op.index] = op.pos;
        break;
      case PathPointOp::Kind::kErase:
        if (op.index < points.size()) {
          points.erase(points.begin() + static_cast<std::ptrdiff_t>(op.index));
        }
        break;
    }
  }
  if (edit.restart) {
    path.segment = 0;
    path.u = 0.0F;
    path.done = false;
  }
  if (edit.loop.has_value()) path.loop = *edit.loop;
  if (edit.entity_pos.has_value()) entity_pos = *edit.entity_pos;
}

}  // namespace se::editor
