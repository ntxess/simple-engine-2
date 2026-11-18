// Copyright 2026 The simple-engine-2 Authors.
//
// The Scene settings panel and the default panel set. Scene settings shows
// the World's primary camera (edited through set-component commands, or
// added as a new entity when there is none: the sandbox then shows the
// whole arena), the tick rate, and the background colour the Viewport
// clears to (an editor setting, not document state).

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/view.h"
#include "se_core/core_components.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/editor_stats.h"
#include "se_editor/panel.h"
#include "se_editor_panels/panel_support.h"
#include "se_editor_panels/panels.h"
#include "se_render_core/render_components.h"

namespace se::editor {
namespace {

// The primary camera's entity (the lowest spawn serial among primaries,
// as PrimaryCamera picks it).
std::optional<entt::entity> PrimaryCameraEntity(const World& world) {
  std::optional<entt::entity> best;
  std::uint32_t best_serial = 0;
  const entt::registry& reg = world.CReg();
  for (const auto [e, camera, spawned] :
       reg.view<const Camera2D, const Spawned>().each()) {
    if (!camera.primary) continue;
    if (!best || spawned.spawn_serial < best_serial) {
      best = e;
      best_serial = spawned.spawn_serial;
    }
  }
  return best;
}

}  // namespace

void SceneSettingsPanel::Draw(const PanelContext& context) {
  if (context.world == nullptr) {
    ImGui::TextDisabled("No document");
    return;
  }
  const World& world = *context.world;
  EditSink sink(context);
  const entt::registry& reg = world.CReg();

  ImGui::SeparatorText("Camera");
  const std::optional<entt::entity> camera_entity = PrimaryCameraEntity(world);
  if (camera_entity) {
    const Guid guid = reg.get<IdComponent>(*camera_entity).guid;
    Camera2D camera = reg.get<Camera2D>(*camera_entity);
    bool changed = false;
    ImGui::BeginDisabled(!sink.CanEdit());
    float center[2] = {camera.center.x, camera.center.y};
    if (ImGui::DragFloat2("Center", center, 1.0F)) {
      camera.center = {center[0], center[1]};
      changed = true;
    }
    ProbeItem("scene.camera.center");
    TrackCoalesce(sink, "Edit camera");
    if (ImGui::DragFloat("Zoom", &camera.zoom, 0.01F, 0.01F, 100.0F)) {
      changed = true;
    }
    ProbeItem("scene.camera.zoom");
    TrackCoalesce(sink, "Edit camera");
    float size[2] = {camera.virtual_size.x, camera.virtual_size.y};
    if (ImGui::DragFloat2("Virtual size", size, 1.0F, 1.0F, 16384.0F)) {
      camera.virtual_size = {size[0], size[1]};
      changed = true;
    }
    TrackCoalesce(sink, "Edit camera");
    if (ImGui::Checkbox("Integer scale", &camera.integer_scale)) changed = true;
    ImGui::EndDisabled();
    if (changed) {
      camera.zoom = std::max(camera.zoom, 0.01F);
      if (auto before = ReadComponentJson(world, *camera_entity, "Camera2D")) {
        sink.Submit(
            SetFromValue(world, guid, "Camera2D", std::move(*before), camera));
      }
    }
  } else {
    const Camera2D arena = rampage::SandboxCamera(world);
    ImGui::TextWrapped(
        "No primary camera: the game shows the whole arena (%.0f x %.0f).",
        arena.virtual_size.x, arena.virtual_size.y);
    ImGui::BeginDisabled(!sink.CanEdit());
    if (ImGui::Button("Add primary camera")) {
      Camera2D camera = arena;
      camera.primary = true;
      auto json = JsonOfValue(world, "Camera2D", camera);
      if (json) {
        std::vector<ComponentJson> components{{"Name", R"({"value":"Camera"})"},
                                              {"Transform", "{}"},
                                              {"Camera2D", std::move(*json)}};
        sink.Submit(std::make_unique<CreateEntityCommand>(
            NewEntityGuid(world, "camera"), std::move(components)));
      }
    }
    ProbeItem("scene.camera.add");
    ImGui::EndDisabled();
  }
  if (const auto* bounds = reg.ctx().find<rampage::ArenaBounds>()) {
    ImGui::Text("Arena: %.0f x %.0f", bounds->w, bounds->h);
  }

  ImGui::SeparatorText("Simulation");
  const SimClock& clock = world.Clock();
  ImGui::Text("Tick rate: %u Hz (dt %.3f ms)", clock.rate, clock.dt * 1e3);
  const std::string tick = std::format("Tick: {}", clock.tick);
  ImGui::TextUnformatted(tick.c_str());
  ProbeItem("scene.tick");
  if (context.stats != nullptr && context.playing) {
    const std::string next =
        std::format("Next tick: {}", context.stats->next_tick);
    ImGui::TextUnformatted(next.c_str());
  }

  ImGui::SeparatorText("View");
  ImGui::ColorEdit4("Background", settings_->background);
  ProbeItem("scene.background");
}

DefaultPanels::DefaultPanels(PanelOptions options) {
  auto hierarchy = std::make_unique<HierarchyPanel>();
  auto inspector = std::make_unique<InspectorPanel>();
  auto viewport = std::make_unique<ViewportPanel>(&view_);
  auto log = std::make_unique<LogPanel>();
  auto profiler =
      std::make_unique<ProfilerPanel>(std::move(options.dump_flight_recorder));
  auto schedule = std::make_unique<SchedulePanel>();
  auto stress = std::make_unique<StressPanel>();
  auto scene_settings = std::make_unique<SceneSettingsPanel>(&view_);
  hierarchy_ = hierarchy.get();
  inspector_ = inspector.get();
  viewport_ = viewport.get();
  log_ = log.get();
  profiler_ = profiler.get();
  schedule_ = schedule.get();
  stress_ = stress.get();
  scene_settings_ = scene_settings.get();
  panels_.push_back(std::move(hierarchy));
  panels_.push_back(std::move(inspector));
  panels_.push_back(std::move(viewport));
  panels_.push_back(std::move(log));
  panels_.push_back(std::move(profiler));
  panels_.push_back(std::move(schedule));
  panels_.push_back(std::move(stress));
  panels_.push_back(std::move(scene_settings));
}

std::vector<std::unique_ptr<Panel>> MakeDefaultPanels(PanelOptions options) {
  auto view = std::make_shared<ViewSettings>();
  std::vector<std::unique_ptr<Panel>> panels;
  panels.push_back(std::make_unique<HierarchyPanel>());
  panels.push_back(std::make_unique<InspectorPanel>());
  panels.push_back(std::make_unique<ViewportPanel>(view));
  panels.push_back(std::make_unique<LogPanel>());
  panels.push_back(
      std::make_unique<ProfilerPanel>(std::move(options.dump_flight_recorder)));
  panels.push_back(std::make_unique<SchedulePanel>());
  panels.push_back(std::make_unique<StressPanel>());
  panels.push_back(std::make_unique<SceneSettingsPanel>(std::move(view)));
  return panels;
}

void DefaultPanels::SetProbe(WidgetProbe* probe) noexcept {
  hierarchy_->SetProbe(probe);
  inspector_->SetProbe(probe);
  viewport_->SetProbe(probe);
  log_->SetProbe(probe);
  profiler_->SetProbe(probe);
  schedule_->SetProbe(probe);
  stress_->SetProbe(probe);
  scene_settings_->SetProbe(probe);
}

}  // namespace se::editor
