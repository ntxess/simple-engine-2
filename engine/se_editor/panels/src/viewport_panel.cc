// Copyright 2026 The simple-engine-2 Authors.
//
// The Viewport panel: an editor camera's view of the World in an M7
// ViewportTarget, picking, the move gizmo and the overlays.

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "game_rampage/view.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/panel.h"
#include "se_editor/selection.h"
#include "se_editor/texture_ids.h"
#include "se_editor_panels/gizmo.h"
#include "se_editor_panels/overlays.h"
#include "se_editor_panels/panel_support.h"
#include "se_editor_panels/panels.h"
#include "se_editor_panels/picking.h"
#include "se_editor_panels/viewport_math.h"
#include "se_render/frame_recorder.h"
#include "se_render/renderer_2d.h"
#include "se_render_core/render_components.h"

namespace se::editor {
namespace {

ImU32 Color(Rgba8 c) { return IM_COL32(c.r, c.g, c.b, c.a); }

ImVec2 Im(Vec2 v) { return {v.x, v.y}; }

}  // namespace

std::optional<Vec2> ViewportPanel::GizmoOrigin(const PanelContext& context,
                                               Guid* guid) const {
  if (context.selection == nullptr || context.selection->Empty()) {
    return std::nullopt;
  }
  const Guid primary = context.selection->Primary();
  const std::optional<entt::entity> e = context.world->Find(primary);
  if (!e) return std::nullopt;
  const auto* transform = context.world->CReg().try_get<Transform>(*e);
  if (transform == nullptr) return std::nullopt;
  if (guid != nullptr) *guid = primary;
  return mapping_.WorldToScreen(transform->pos);
}

void ViewportPanel::Draw(const PanelContext& context) {
  if (context.world == nullptr) {
    ImGui::TextDisabled("No document");
    return;
  }
  const World& world = *context.world;
  EditSink sink(context);
  if (!camera_) {
    // The view the game shows (its primary camera, else the arena), as an
    // editor camera the panel pans and zooms on its own.
    Camera2D camera = rampage::SandboxCamera(world);
    camera.primary = false;
    camera_ = camera;
  }

  // Toolbar: the legacy "Scene View Option" checkboxes (Editor.cpp:265-276).
  OverlayOptions& overlays = settings_->overlays;
  ImGui::Checkbox("IDs", &overlays.ids);
  ProbeItem("viewport.ids");
  ImGui::SameLine();
  ImGui::Checkbox("Boxes", &overlays.boxes);
  ImGui::SameLine();
  ImGui::Checkbox("Headings", &overlays.headings);
  ImGui::SameLine();
  ImGui::Checkbox("Positions", &overlays.positions);
  ImGui::SameLine();
  if (ImGui::Button("Reset view")) {
    Camera2D camera = rampage::SandboxCamera(world);
    camera.primary = false;
    camera_ = camera;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("zoom %.2f", camera_->zoom);

  const ImVec2 cursor = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  mapping_ = MapViewport({cursor.x, cursor.y}, {avail.x, avail.y}, *camera_);
  const ImVec2 image_min = Im(mapping_.image_min);
  const ImVec2 image_size{static_cast<float>(mapping_.target_width),
                          static_cast<float>(mapping_.target_height)};

  // The World as the renderer draws it this frame (M10a's EditorViewports;
  // none in a headless editor), resolved through TextureIds every frame.
  if (context.viewports != nullptr) {
    if (ViewportTarget* target = context.viewports->Viewport("scene")) {
      if (target->Width() != mapping_.target_width ||
          target->Height() != mapping_.target_height) {
        target->RequestSize(mapping_.target_width, mapping_.target_height);
      }
      target->SetCamera(*camera_);
      target->SetClearColor({settings_->background[0], settings_->background[1],
                             settings_->background[2],
                             settings_->background[3]});
      if (context.textures != nullptr) {
        ImGui::SetCursorScreenPos(image_min);
        ImGui::Image(context.textures->Id(target->TargetTexture()), image_size);
      }
    }
  }

  ImGui::SetCursorScreenPos(image_min);
  ImGui::InvisibleButton("##viewport", image_size,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonRight |
                             ImGuiButtonFlags_MouseButtonMiddle);
  ProbeItem("viewport.image");
  HandleInput(context, sink);
  DrawOverlays(context);
}

void ViewportPanel::HandleInput(const PanelContext& context, EditSink& sink) {
  const World& world = *context.world;
  const ImGuiIO& io = ImGui::GetIO();
  const Vec2 mouse{io.MousePos.x, io.MousePos.y};
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();

  // Pan and zoom the editor camera.
  if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
                 ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
    camera_ = PanCamera(*camera_, {io.MouseDelta.x, io.MouseDelta.y},
                        mapping_.Scale());
  }
  if (hovered && io.MouseWheel != 0.0F) {
    camera_ = ZoomCamera(mapping_, mouse, io.MouseWheel);
  }

  // A press on a gizmo handle starts a drag; elsewhere a click picks.
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    Guid guid;
    const std::optional<Vec2> origin = GizmoOrigin(context, &guid);
    const GizmoAxis axis =
        origin && sink.CanEdit() ? HitGizmo(*origin, mouse) : GizmoAxis::kNone;
    if (axis != GizmoAxis::kNone) {
      const entt::entity e = *world.Find(guid);
      drag_ = Drag{axis, guid, world.CReg().get<Transform>(e).pos, mouse};
      sink.BeginCoalesce(std::format("Move {}", EntityName(world, e)));
    } else {
      scratch_.Refresh(world);
      last_pick_ = Pick(world, mapping_.ScreenToWorld(mouse), context.faulted,
                        &scratch_);
      std::vector<Guid> next;
      if (io.KeyCtrl && context.selection != nullptr) {
        next.assign(context.selection->Guids().begin(),
                    context.selection->Guids().end());
      }
      if (!last_pick_.Empty()) {
        const Guid hit = last_pick_.hits.front().guid;
        const auto it =
            std::find_if(next.begin(), next.end(),
                         [&hit](const Guid& guid) { return guid == hit; });
        if (io.KeyCtrl && it != next.end()) {
          next.erase(it);
        } else {
          next.push_back(hit);
        }
      }
      sink.Select(next);
    }
  }

  if (drag_) {
    const std::optional<entt::entity> e = world.Find(drag_->guid);
    if (e && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const auto& transform = world.CReg().get<Transform>(*e);
      const Vec2 pos =
          GizmoDragPosition(drag_->axis, drag_->start_pos, drag_->start_mouse,
                            mouse, mapping_.Scale());
      if (pos != transform.pos) {
        if (auto before = ReadComponentJson(world, *e, "Transform")) {
          Transform moved = transform;
          moved.pos = pos;
          sink.Submit(SetFromValue(world, drag_->guid, "Transform",
                                   std::move(*before), moved));
        }
      }
    }
    if (!e || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      drag_.reset();
      sink.EndCoalesce();
    }
  }
}

void ViewportPanel::DrawOverlays(const PanelContext& context) {
  const World& world = *context.world;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 clip_min = Im(mapping_.image_min);
  const ImVec2 clip_max{
      clip_min.x + static_cast<float>(mapping_.target_width),
      clip_min.y + static_cast<float>(mapping_.target_height)};
  draw->PushClipRect(clip_min, clip_max, true);

  BuildOverlays(world, settings_->overlays, mapping_, overlays_);
  for (const OverlayQuad& quad : overlays_.quads) {
    const ImVec2 points[4] = {Im(quad.corners[0]), Im(quad.corners[1]),
                              Im(quad.corners[2]), Im(quad.corners[3])};
    draw->AddPolyline(points, 4, Color(quad.color), 2.0F, ImDrawFlags_Closed);
  }
  for (const OverlayLine& line : overlays_.lines) {
    draw->AddLine(Im(line.from), Im(line.to), Color(line.color), 2.0F);
  }
  for (const OverlayText& text : overlays_.texts) {
    draw->AddText(Im(text.pos), Color(text.color), text.text.c_str());
  }

  // The selection's sprite bounds, then the gizmo of the primary one.
  if (context.selection != nullptr) {
    const entt::registry& reg = world.CReg();
    for (const Guid& guid : context.selection->Guids()) {
      const std::optional<entt::entity> e = world.Find(guid);
      if (!e) continue;
      const auto* transform = reg.try_get<Transform>(*e);
      const auto* sprite = reg.try_get<SpriteRenderer>(*e);
      if (transform == nullptr || sprite == nullptr) continue;
      const PickBounds bounds = SpriteBounds(*transform, *sprite);
      draw->AddRect(Im(mapping_.WorldToScreen(bounds.min)),
                    Im(mapping_.WorldToScreen(bounds.max)),
                    IM_COL32(255, 220, 0, 255), 0.0F, 1.5F);
    }
  }
  if (const std::optional<Vec2> origin = GizmoOrigin(context, nullptr)) {
    const ImVec2 o = Im(*origin);
    const ImGuiIO& io = ImGui::GetIO();
    const GizmoAxis hot =
        drag_ ? drag_->axis : HitGizmo(*origin, {io.MousePos.x, io.MousePos.y});
    const auto shade = [hot](GizmoAxis axis, ImU32 color) {
      return hot == axis ? IM_COL32(255, 255, 255, 255) : color;
    };
    draw->AddLine(o, {o.x + kGizmoLength, o.y},
                  shade(GizmoAxis::kX, IM_COL32(230, 60, 60, 255)), 3.0F);
    draw->AddTriangleFilled({o.x + kGizmoLength + 8.0F, o.y},
                            {o.x + kGizmoLength, o.y - 5.0F},
                            {o.x + kGizmoLength, o.y + 5.0F},
                            shade(GizmoAxis::kX, IM_COL32(230, 60, 60, 255)));
    draw->AddLine(o, {o.x, o.y + kGizmoLength},
                  shade(GizmoAxis::kY, IM_COL32(60, 200, 60, 255)), 3.0F);
    draw->AddTriangleFilled({o.x, o.y + kGizmoLength + 8.0F},
                            {o.x - 5.0F, o.y + kGizmoLength},
                            {o.x + 5.0F, o.y + kGizmoLength},
                            shade(GizmoAxis::kY, IM_COL32(60, 200, 60, 255)));
    draw->AddRectFilled({o.x - kGizmoCenterHalf, o.y - kGizmoCenterHalf},
                        {o.x + kGizmoCenterHalf, o.y + kGizmoCenterHalf},
                        shade(GizmoAxis::kBoth, IM_COL32(240, 200, 40, 200)));
  }
  draw->PopClipRect();
}

}  // namespace se::editor
