// Copyright 2026 The simple-engine-2 Authors.
//
// The Inspector panel and its waypoint canvas window.

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/view.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/panel.h"
#include "se_editor/prefab_commands.h"
#include "se_editor/selection.h"
#include "se_editor/texture_ids.h"
#include "se_editor_panels/component_drawers.h"
#include "se_editor_panels/panel_support.h"
#include "se_editor_panels/panels.h"
#include "se_editor_panels/path_commands.h"
#include "se_editor_panels/viewport_math.h"
#include "se_editor_panels/waypoint_canvas.h"
#include "se_render/renderer_2d.h"
#include "se_render_core/render_components.h"

namespace se::editor {
namespace {

// The prefab link is shown as a line of its own, not as a component.
constexpr std::string_view kHidden[] = {"IdComponent", "DestroyRequest",
                                        kPrefabInstanceComponent};

bool Hidden(std::string_view name) {
  for (const std::string_view hidden : kHidden) {
    if (name == hidden) return true;
  }
  return false;
}

void SubmitPathEdit(EditSink& sink, const World& world, Guid guid,
                    const std::optional<PathEdit>& edit) {
  if (!edit) return;
  auto commands = PathEditCommands(world, guid, *edit);
  if (!commands) {
    EditSink::ReportError(commands.error());
    return;
  }
  sink.BeginCoalesce(PathEditLabel(edit->kind));
  for (auto& command : *commands) sink.Submit(std::move(command));
  sink.EndCoalesce();
}

ImU32 PointColor(std::size_t i, std::size_t count) {
  if (i == 0) return IM_COL32(0, 255, 0, 255);
  if (i + 1 == count) return IM_COL32(255, 0, 0, 255);
  return IM_COL32(255, 255, 255, 255);
}

}  // namespace

std::optional<CanvasView> InspectorPanel::CanvasViewOf(const Guid& guid) const {
  const auto it = canvases_.find(guid);
  if (it == canvases_.end()) return std::nullopt;
  return it->second.view;
}

const EntityJson* InspectorPanel::Baseline(const PanelContext& context,
                                           Guid guid) {
  return baselines_.Get(context, guid);
}

void InspectorPanel::Draw(const PanelContext& context) {
  overridden_.clear();
  if (context.world == nullptr) {
    ImGui::TextDisabled("No document");
    return;
  }
  if (context.selection == nullptr || context.selection->Empty()) {
    ImGui::TextDisabled("Nothing selected");
    return;
  }
  const Guid guid = context.selection->Primary();
  const std::optional<entt::entity> entity = context.world->Find(guid);
  if (!entity) {
    ImGui::TextDisabled("The selection is not in this World");
    return;
  }
  EditSink sink(context);
  DrawEntity(context, sink, *entity, guid);
}

void InspectorPanel::DrawEntity(const PanelContext& context, EditSink& sink,
                                entt::entity entity, Guid guid) {
  const World& world = *context.world;
  const std::string_view name = EntityName(world, entity);
  ImGui::Text("%s", name.empty() ? "(unnamed)" : std::string(name).c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("%s", ToString(guid).c_str());
  if (context.faulted) {
    ImGui::TextColored({1.0F, 0.4F, 0.4F, 1.0F},
                       "Play faulted: the World is read-only");
  } else if (context.playing) {
    ImGui::TextDisabled("Playing: edits apply at the next tick");
  }
  // A prefab instance: its prefab, and what differs from a bare instance.
  const std::optional<std::string> prefab = PrefabOf(world, guid);
  const EntityJson* baseline = Baseline(context, guid);
  if (prefab) {
    ImGui::TextDisabled("Prefab: %s", prefab->c_str());
    ProbeItem("inspector.prefab");
  }
  if (baseline != nullptr) {
    overridden_ = OverriddenComponents(world, guid, *baseline);
  }
  const auto overridden = [this](std::string_view name) {
    for (const std::string& o : overridden_) {
      if (o == name) return true;
    }
    return false;
  };
  const auto revert = [&](std::string_view name) {
    if (baseline == nullptr) return;
    auto command = MakeRevertToPrefab(world, guid, name, *baseline);
    if (!command) {
      EditSink::ReportError(command.error());
      return;
    }
    sink.Submit(std::move(*command));
  };
  // Components of the prefab this instance removed.
  if (baseline != nullptr) {
    for (const std::string& removed : overridden_) {
      const ComponentInfo* info = world.Registry().FindByName(removed);
      if (info == nullptr || HasComponent(world, entity, *info)) continue;
      ImGui::PushID(removed.c_str());
      ImGui::TextColored({1.0F, 0.75F, 0.3F, 1.0F}, "%s: removed [override]",
                         removed.c_str());
      ProbeItem("inspector.override." + removed);
      ImGui::SameLine();
      ImGui::BeginDisabled(!sink.CanEdit());
      if (ImGui::SmallButton("Revert to prefab")) revert(removed);
      ProbeItem("inspector.revert." + removed);
      ImGui::EndDisabled();
      ImGui::PopID();
    }
  }

  DrawTarget target;
  target.world = &world;
  target.entity = entity;
  target.guid = guid;
  target.sink = &sink;
  target.text = &text_;
  target.probe = Probe();
  target.read_only = !sink.CanEdit();

  std::vector<const ComponentInfo*> others;
  std::vector<const ComponentInfo*> addable;
  for (const ComponentInfo& info : world.Registry().All()) {
    if (Hidden(info.name)) continue;
    const bool has = HasComponent(world, entity, info);
    const bool visible = HasFlag(info.flags, ComponentFlags::kEditorVisible);
    if (!has) {
      if (visible && HasFlag(info.flags, ComponentFlags::kSerialized)) {
        addable.push_back(&info);
      }
      continue;
    }
    if (!visible) {
      others.push_back(&info);
      continue;
    }
    ImGui::PushID(info.name.data(), info.name.data() + info.name.size());
    bool keep = true;
    const std::string header(info.name);
    const bool marked = overridden(header);
    // The label changes with the marker; the id stays the component's.
    const std::string label =
        std::format("{}{}###{}", header, marked ? "  [override]" : "", header);
    // What the game keeps (the entity's identity, its prefab link, its
    // place) has no close button.
    const bool removable =
        sink.CanEdit() &&
        (context.game == nullptr || RemovableBy(*context.game, header));
    const bool open =
        ImGui::CollapsingHeader(label.c_str(), removable ? &keep : nullptr,
                                ImGuiTreeNodeFlags_DefaultOpen);
    ProbeItem("inspector.header." + header);
    if (!keep) {
      // The component and what derives from it: one undo step.
      if (context.game != nullptr) {
        sink.Submit(MakeRemoval(world, guid, header, *context.game));
      } else {
        sink.Submit(std::make_unique<RemoveComponentCommand>(guid, header));
      }
    } else if (open) {
      if (marked) {
        ImGui::TextColored({1.0F, 0.75F, 0.3F, 1.0F},
                           "Differs from the prefab");
        ProbeItem("inspector.override." + header);
        ImGui::SameLine();
        ImGui::BeginDisabled(!sink.CanEdit());
        if (ImGui::SmallButton("Revert to prefab")) revert(header);
        ProbeItem("inspector.revert." + header);
        ImGui::EndDisabled();
      }
      DrawComponent(target, info);
    }
    ImGui::PopID();
  }

  if (!others.empty() &&
      ImGui::CollapsingHeader("Other components (read-only)")) {
    DrawTarget read_only = target;
    read_only.read_only = true;
    for (const ComponentInfo* info : others) {
      ImGui::PushID(info->name.data(), info->name.data() + info->name.size());
      if (ImGui::TreeNode(std::string(info->name).c_str())) {
        reflection_.Draw(read_only, *info);
        ImGui::TreePop();
      }
      ImGui::PopID();
    }
  }

  if (sink.CanEdit() && !addable.empty()) {
    ImGui::Spacing();
    if (ImGui::BeginCombo("##add_component", "Add component")) {
      for (const ComponentInfo* info : addable) {
        const std::string label(info->name);
        if (ImGui::Selectable(label.c_str())) {
          sink.Submit(std::make_unique<AddComponentCommand>(guid, label, "{}"));
        }
      }
      ImGui::EndCombo();
    }
    ProbeItem("inspector.add");
  }

  for (auto& [canvas_guid, canvas] : canvases_) {
    if (canvas_guid == guid && canvas.open) {
      DrawCanvasWindow(target, context, canvas);
    }
  }
}

void InspectorPanel::DrawComponent(const DrawTarget& target,
                                   const ComponentInfo& info) {
  if (info.name == "Stats") {
    stats_.Draw(target);
  } else if (info.name == "EffectSource") {
    effects_.Draw(target);
  } else if (info.name == "Team") {
    DrawTeam(target);
  } else if (info.name == "Path") {
    DrawPath(target);
  } else {
    reflection_.Draw(target, info);
  }
}

void InspectorPanel::DrawPath(const DrawTarget& target) {
  // The legacy MovementPattern visitor (EditorComponentVisitor.cpp:198-243
  // at master): each point with the distance to the next and the running
  // total, the segment being run highlighted with the distance done; then
  // Repeat, and the button that opens the canvas.
  const World& world = *target.world;
  const entt::registry& reg = world.CReg();
  const auto* path = reg.try_get<rampage::Path>(target.entity);
  if (path == nullptr) return;
  const auto* cache = reg.try_get<rampage::PathCache>(target.entity);
  const bool cached = cache != nullptr && !path->points.empty() &&
                      cache->totals.size() == path->points.size() &&
                      cache->lengths.size() + 1 >= path->points.size();

  ImGui::BeginDisabled(target.read_only);
  if (ImGui::Button("Set New Path", {ImGui::GetContentRegionAvail().x, 0})) {
    OpenCanvas(target.guid);
  }
  ProbeItem("path.open_canvas");
  const float width = ImGui::GetContentRegionAvail().x;
  ImGui::TextUnformatted("Way Point");
  ImGui::SameLine(width / 3.0F);
  ImGui::TextUnformatted("Distance To Next");
  ImGui::SameLine(width * 2.0F / 3.0F);
  ImGui::TextUnformatted("Total Distance");
  for (std::size_t i = 0; i < path->points.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    const bool running = i == path->segment && !path->done;
    if (running) {
      ImGui::TextColored({0.0F, 1.0F, 0.0F, 1.0F}, "%zu  Run %.2f", i, path->u);
    } else {
      ImGui::Text("%zu", i);
    }
    ImGui::SameLine(width / 3.0F);
    if (cached && i < cache->lengths.size()) {
      ImGui::Text("%.2f", cache->lengths[i]);
    } else {
      ImGui::TextDisabled("-");
    }
    ImGui::SameLine(width * 2.0F / 3.0F);
    if (cached) {
      ImGui::Text("%.2f", cache->totals[i]);
    } else {
      ImGui::TextDisabled("-");
    }
    float xy[2] = {path->points[i].x, path->points[i].y};
    ImGui::SetNextItemWidth(width / 2.0F);
    if (ImGui::DragFloat2("##point", xy, 0.5F)) {
      target.sink->Submit(PathPointCommand::Move(
          world, target.guid, static_cast<std::uint32_t>(i), {xy[0], xy[1]}));
    }
    ProbeItem(std::format("path.point.{}", i));
    TrackCoalesce(*target.sink, "Move path point");
    ImGui::PopID();
  }
  bool loop = path->loop;
  if (ImGui::Checkbox("Repeat", &loop)) {
    auto before = ReadComponentJson(world, target.entity, "Path");
    if (before) {
      rampage::Path after = *path;
      after.loop = loop;
      target.sink->Submit(
          SetFromValue(world, target.guid, "Path", std::move(*before), after));
    }
  }
  ProbeItem("path.repeat");
  ImGui::EndDisabled();
}

void InspectorPanel::DrawCanvasWindow(const DrawTarget& target,
                                      const PanelContext& context,
                                      Canvas& canvas) {
  const World& world = *target.world;
  const entt::registry& reg = world.CReg();
  const auto* path = reg.try_get<rampage::Path>(target.entity);
  const auto* transform = reg.try_get<Transform>(target.entity);
  const std::string name(EntityName(world, target.entity));
  const std::string title =
      std::format("WayPoint Editor | {}###waypoint_canvas", name);
  ImGui::SetNextWindowSize({640.0F, 400.0F}, ImGuiCond_FirstUseEver);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
  const bool visible = ImGui::Begin(
      title.c_str(), &canvas.open,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  if (!visible || path == nullptr || transform == nullptr) {
    ImGui::End();
    return;
  }

  // The canvas shows the whole arena (the sandbox's view), letterboxed
  // into the window as the legacy scene view was (Editor.cpp:684-690).
  const Camera2D arena = rampage::SandboxCamera(world);
  const Vec2 cursor{ImGui::GetCursorScreenPos().x,
                    ImGui::GetCursorScreenPos().y};
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const Vec2 region{avail.x, avail.y};
  const float aspect = arena.virtual_size.y > 0.0F
                           ? arena.virtual_size.x / arena.virtual_size.y
                           : 0.0F;
  const Vec2 size = FitAspectSize(region, aspect);
  const Vec2 top_left = CenteredTopLeft(cursor, region, aspect);
  const Vec2 draw_scale{
      arena.virtual_size.x > 0.0F ? size.x / arena.virtual_size.x : 1.0F,
      arena.virtual_size.y > 0.0F ? size.y / arena.virtual_size.y : 1.0F};
  const CanvasMapping mapping = MapCanvas(top_left, draw_scale, canvas.view);

  // Input first, with this frame's mapping (Editor.cpp:903-930).
  ImGui::SetCursorScreenPos({top_left.x, top_left.y});
  ImGui::InvisibleButton(
      "canvas", {std::max(size.x, 1.0F), std::max(size.y, 1.0F)},
      ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  ProbeItem("canvas");
  const ImGuiIO& io = ImGui::GetIO();
  CanvasInput input;
  input.mouse = {io.MousePos.x, io.MousePos.y};
  input.mouse_delta = {io.MouseDelta.x, io.MouseDelta.y};
  input.hovered = ImGui::IsItemHovered();
  input.active = ImGui::IsItemActive();
  input.left_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  input.right_dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Right);
  const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
  input.right_drag_delta = {drag.x, drag.y};
  input.right_released = ImGui::IsMouseReleased(ImGuiMouseButton_Right);
  input.wheel = io.MouseWheel;
  const CanvasFrame frame = ProcessCanvasInput(canvas.view, mapping, input);
  canvas.view = frame.view;
  if (frame.open_menu) ImGui::OpenPopup("context");
  if (frame.add_point && !target.read_only) {
    SubmitPathEdit(*target.sink, world, target.guid,
                   AppendPointEdit(*path, transform->pos, *frame.add_point));
  }

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 clip_min{top_left.x, top_left.y};
  const ImVec2 clip_max{top_left.x + size.x, top_left.y + size.y};
  draw->PushClipRect(clip_min, clip_max, true);

  // The scene behind the canvas, panned and zoomed with it (Editor.cpp:
  // 695-698): an offscreen viewport of the arena.
  if (context.viewports != nullptr && context.textures != nullptr) {
    if (ViewportTarget* scene =
            context.viewports->Viewport("waypoint_canvas")) {
      const auto w = static_cast<std::uint32_t>(std::max(size.x, 1.0F));
      const auto h = static_cast<std::uint32_t>(std::max(size.y, 1.0F));
      if (scene->Width() != w || scene->Height() != h) scene->RequestSize(w, h);
      scene->SetCamera(arena);
      const ImVec2 image_min{mapping.origin.x, mapping.origin.y};
      const ImVec2 image_max{mapping.origin.x + size.x * canvas.view.zoom,
                             mapping.origin.y + size.y * canvas.view.zoom};
      draw->AddImage(context.textures->Id(scene->TargetTexture()), image_min,
                     image_max);
    }
  }

  // The grid (Editor.cpp:711-731).
  std::vector<float> lines;
  CanvasGridLines(canvas.view.scrolling.x, canvas.view.zoom, size.x, lines);
  for (const float x : lines) {
    draw->AddLine({top_left.x + x, top_left.y},
                  {top_left.x + x, top_left.y + size.y},
                  IM_COL32(200, 200, 200, 40));
  }
  CanvasGridLines(canvas.view.scrolling.y, canvas.view.zoom, size.y, lines);
  for (const float y : lines) {
    draw->AddLine({top_left.x, top_left.y + y},
                  {top_left.x + size.x, top_left.y + y},
                  IM_COL32(200, 200, 200, 40));
  }

  // The path and its labels (Editor.cpp:733-770): yellow segments, the
  // first label green, the last red.
  const std::size_t count = path->points.size();
  for (std::size_t i = 0; i < count; ++i) {
    const Vec2 p = WorldToCanvas(mapping, path->points[i]);
    if (i + 1 < count) {
      const Vec2 q = WorldToCanvas(mapping, path->points[i + 1]);
      draw->AddLine({p.x, p.y}, {q.x, q.y}, IM_COL32(255, 255, 0, 255), 2.0F);
    }
    const std::string label =
        std::format("({:.2f}, {:.2f})", path->points[i].x, path->points[i].y);
    draw->AddText({p.x, p.y}, PointColor(i, count), label.c_str());
  }
  draw->PopClipRect();

  if (ImGui::BeginPopup("context")) {
    // Editor.cpp:774-801.
    if (ImGui::MenuItem("Remove one", nullptr, false,
                        count > 0 && !target.read_only)) {
      SubmitPathEdit(*target.sink, world, target.guid,
                     RemoveLastPointEdit(*path, transform->pos));
    }
    ProbeItem("canvas.menu.remove_one");
    if (ImGui::MenuItem("Remove all", nullptr, false,
                        count > 0 && !target.read_only)) {
      SubmitPathEdit(*target.sink, world, target.guid,
                     ClearPathEdit(*path, transform->pos));
    }
    ProbeItem("canvas.menu.remove_all");
    if (ImGui::MenuItem("Reset Camera")) canvas.view = ResetCanvasView();
    ProbeItem("canvas.menu.reset");
    ImGui::EndPopup();
  }

  ImGui::SetCursorScreenPos(clip_min);
  ImGui::TextUnformatted(
      " Mouse Left: click to add waypoints, Mouse Right: drag to pan, click "
      "for context menu.");
  ImGui::End();
}

}  // namespace se::editor
