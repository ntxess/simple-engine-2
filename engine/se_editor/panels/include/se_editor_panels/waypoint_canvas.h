// Copyright 2026 The simple-engine-2 Authors.
//
// The waypoint canvas of the inspector's Path drawer, ported from the legacy
// editor (app/editor/Editor.cpp:677-931 at master; docs/design/
// costfree-end-state.md "Kept From Todays Code"). The ImGui half lives in
// the Inspector; this header holds what does not need ImGui: the canvas
// mapping, its controls (:903-930) and its path-editing rules (:828-885),
// each a pure function the tests drive directly.
//
// Controls, as the legacy canvas had them:
//   - left click (hovered) adds a point at the cursor;
//   - right drag (the item active, past ImGui's drag threshold) pans: the
//     scrolling follows the mouse delta;
//   - a right click that did not drag opens the context menu ("Remove
//     one", "Remove all", "Reset Camera");
//   - the wheel (hovered) zooms in 0.1 steps, clamped to [0.1, 10].
// The cursor's world position is taken with the scrolling before this
// frame's pan, as the legacy code computed it first.
//
// Path-editing rules. The legacy canvas kept its own copy of the points and
// synced it with the WayPoint list; the engine canvas draws the Path
// component itself every frame (no mirror), and each rule becomes a PathEdit
// that the Inspector submits as commands (ADR-100):
//   - Append (left click): a Path without points first takes the entity's
//     position as its point 0 (the canvas sync at :830-834, so the first
//     point stays where the entity starts); the progress restarts and the
//     entity moves to point 0 (:846-850); the point is appended (:860-870).
//   - RemoveLast ("Remove one", only when the Path has points): the
//     progress restarts and the entity moves to point 0; the last point is
//     erased, except that a Path keeps its last remaining point (:871-883:
//     the lone node was replaced by one at the entity's position, which
//     had just been moved onto it).
//   - Clear ("Remove all", only when the Path has points): the Path becomes
//     the entity's current position alone, the progress restarts and the
//     Path loops (:836-843 re-created the MovementPattern with repeat =
//     true); the entity does not move.
// "Restart" is the legacy MovementPattern::restart(): Path::segment = 0,
// u = 0, done = false.

#ifndef SE_EDITOR_PANELS_WAYPOINT_CANVAS_H_
#define SE_EDITOR_PANELS_WAYPOINT_CANVAS_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "game_rampage/components.h"
#include "se_core/types.h"

namespace se::editor {

inline constexpr float kCanvasZoomStep = 0.1F;
inline constexpr float kCanvasZoomMin = 0.1F;
inline constexpr float kCanvasZoomMax = 10.0F;
// The grid's spacing in canvas pixels at zoom 1 (Editor.cpp:716).
inline constexpr float kCanvasGridStep = 32.0F;

// A canvas's camera: panel state, not document state (the legacy
// ComponentPropData::scrolling and zoom).
struct CanvasView {
  Vec2 scrolling;
  float zoom = 1.0F;
  friend constexpr bool operator==(const CanvasView&,
                                   const CanvasView&) = default;
};

// Where world pixels land on screen: screen = origin + world * draw_scale *
// zoom, with origin = top_left + scrolling (Editor.cpp:690, 741-748).
struct CanvasMapping {
  Vec2 origin;
  Vec2 draw_scale{1.0F, 1.0F};
  float zoom = 1.0F;
};

[[nodiscard]] CanvasMapping MapCanvas(Vec2 top_left, Vec2 draw_scale,
                                      const CanvasView& view) noexcept;
[[nodiscard]] Vec2 CanvasToWorld(const CanvasMapping& mapping,
                                 Vec2 screen) noexcept;
[[nodiscard]] Vec2 WorldToCanvas(const CanvasMapping& mapping,
                                 Vec2 world) noexcept;

// The zoom after |wheel| notches (Editor.cpp:922-929).
[[nodiscard]] float ZoomAfterWheel(float zoom, float wheel) noexcept;

// What ImGui reported about the canvas item this frame.
struct CanvasInput {
  Vec2 mouse;
  Vec2 mouse_delta;
  bool hovered = false;
  bool active = false;
  bool left_clicked = false;
  // ImGui::IsMouseDragging(ImGuiMouseButton_Right) with the default
  // threshold.
  bool right_dragging = false;
  // ImGui::GetMouseDragDelta(ImGuiMouseButton_Right): zero until the drag
  // passes the threshold.
  Vec2 right_drag_delta;
  // The right button was released over the item this frame (when
  // ImGui::OpenPopupOnItemClick fires).
  bool right_released = false;
  float wheel = 0.0F;
};

// The outcome of one frame's input.
struct CanvasFrame {
  CanvasView view;
  // A left click's world position: the Append rule's point.
  std::optional<Vec2> add_point;
  bool open_menu = false;
};

// Applies the controls to |view| (Editor.cpp:903-930).
[[nodiscard]] CanvasFrame ProcessCanvasInput(const CanvasView& view,
                                             const CanvasMapping& mapping,
                                             const CanvasInput& input) noexcept;

// "Reset Camera" (Editor.cpp:794-798).
[[nodiscard]] constexpr CanvasView ResetCanvasView() noexcept { return {}; }

// The grid lines' offsets from the canvas's top-left corner, in screen
// pixels along one axis of |extent| pixels (Editor.cpp:716-731): starting
// at fmod(scrolling * zoom, step * zoom), every step * zoom.
void CanvasGridLines(float scrolling, float zoom, float extent,
                     std::vector<float>& out);

enum class PathEditKind : std::uint8_t { kAppend, kRemoveLast, kClear };

// One change to Path::points, applied in order.
struct PathPointOp {
  enum class Kind : std::uint8_t { kInsert, kMove, kErase };
  Kind kind = Kind::kInsert;
  std::uint32_t index = 0;
  // kInsert and kMove only.
  Vec2 pos;
  friend constexpr bool operator==(const PathPointOp&,
                                   const PathPointOp&) = default;
};

// What a rule does to an entity's Path and Transform.
struct PathEdit {
  PathEditKind kind = PathEditKind::kAppend;
  std::vector<PathPointOp> ops;
  // Path::segment = 0, u = 0, done = false. Every rule restarts.
  bool restart = true;
  // A new Path::loop, when the rule sets one.
  std::optional<bool> loop;
  // A new Transform::pos, when the rule moves the entity.
  std::optional<Vec2> entity_pos;
};

// The rules; nullopt when the rule does nothing (its menu item is
// disabled).
[[nodiscard]] PathEdit AppendPointEdit(const rampage::Path& path,
                                       Vec2 entity_pos, Vec2 point);
[[nodiscard]] std::optional<PathEdit> RemoveLastPointEdit(
    const rampage::Path& path, Vec2 entity_pos);
[[nodiscard]] std::optional<PathEdit> ClearPathEdit(const rampage::Path& path,
                                                    Vec2 entity_pos);

// Applies |edit| to copies of the components (the commands' after values,
// and the tests' expectations). Out-of-range point ops are ignored.
void ApplyPathEdit(const PathEdit& edit, rampage::Path& path, Vec2& entity_pos);

}  // namespace se::editor

#endif  // SE_EDITOR_PANELS_WAYPOINT_CANVAS_H_
