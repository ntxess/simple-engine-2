// Copyright 2026 The simple-engine-2 Authors.
//
// The Stress generator panel: the legacy Debug panel's entity controls
// (app/editor/Editor.cpp:278-333 at master): an amount slider and Replace,
// Generate and Remove. Remove destroys every entity on the enemy team (the
// legacy buttons destroyed every ENEMY TeamTag). A press is planned at once
// (stress_generator.h): in Play, its batches are queued in the SessionLog
// against consecutive ticks; in Edit mode it is one CommandGroup of
// create-entity commands submitted in the frame of the press (ADR-121), so
// no coalesce scope spans frames and a hidden tab leaves nothing open.

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "se_core/core_components.h"
#include "se_core/log.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/panel.h"
#include "se_editor_panels/panel_support.h"
#include "se_editor_panels/panels.h"
#include "se_editor_panels/stress_generator.h"

namespace se::editor {
namespace {

std::vector<ComponentJson> ToCommandComponents(const StressSpawn& spawn) {
  std::vector<ComponentJson> components;
  components.reserve(spawn.components.size());
  for (const SpawnComponent& c : spawn.components) {
    components.push_back({c.component, c.json});
  }
  return components;
}

}  // namespace

std::unique_ptr<EditorCommand> StressPressGroup(
    std::string label, const std::vector<StressSpawn>& spawns) {
  auto group = std::make_unique<CommandGroup>(std::move(label));
  for (const StressSpawn& spawn : spawns) {
    group->Add(std::make_unique<CreateEntityCommand>(
        spawn.guid, ToCommandComponents(spawn)));
  }
  return group;
}

std::vector<StressSpawn> PlanStressFor(const World& world, std::uint32_t press,
                                       std::uint32_t count) {
  float width = 1920.0F;
  float height = 1080.0F;
  if (const auto* bounds = world.CReg().ctx().find<rampage::ArenaBounds>()) {
    width = bounds->w;
    height = bounds->h;
  }
  return PlanStress(DefaultStressSpec(width, height), world.Desc().seed, press,
                    count);
}

std::uint32_t FreeStressPress(const World& world, std::uint32_t from) {
  const std::uint64_t seed = world.Desc().seed;
  while (world.Find(StressGuid(seed, from, 0)).has_value()) ++from;
  return from;
}

std::unique_ptr<EditorCommand> MakeStressPress(const World& world,
                                               std::uint32_t count) {
  return StressPressGroup(std::format("Generate {} enemies", count),
                          PlanStressFor(world, FreeStressPress(world), count));
}

void StressPanel::Generate(const PanelContext& context, EditSink& sink) {
  const World& world = *context.world;
  // Presses already in the World (an earlier session's, saved) are skipped,
  // so their Guids never collide.
  press_ = FreeStressPress(world, press_);
  const auto count = static_cast<std::uint32_t>(std::max(amount_, 0));
  const std::vector<StressSpawn> spawns = PlanStressFor(world, press_, count);
  const std::uint32_t press = press_++;
  if (spawns.empty()) return;
  if (sink.Playing()) {
    // Spread over ticks through the SessionLog: a large press replays
    // exactly.
    const std::vector<SpawnBatch> batches =
        SpreadSpawns(count, static_cast<std::uint32_t>(std::max(per_tick_, 1)));
    const Tick first = sink.NextTick();
    for (const SpawnBatch& batch : batches) {
      for (std::uint32_t i = batch.first; i < batch.first + batch.count; ++i) {
        sink.Submit(std::make_unique<CreateEntityCommand>(
                        spawns[i].guid, ToCommandComponents(spawns[i])),
                    first + batch.offset);
      }
    }
    SE_LOG_INFO(
        "stress generator: press {}: {} enemies over {} ticks from tick {}",
        press, count, batches.size(), first);
    return;
  }
  // Edit mode: the whole press in this frame, one undo step.
  sink.Submit(
      StressPressGroup(std::format("Generate {} enemies", count), spawns));
  SE_LOG_INFO(
      "stress generator: press {}: {} enemies (instances of {}) in one "
      "command group",
      press, count, kStressPrefab);
}

void StressPanel::RemoveEnemies(const PanelContext& context, EditSink& sink) {
  const World& world = *context.world;
  std::vector<Guid> enemies;
  for (const auto [e, team, id] :
       world.CReg().view<const rampage::Team, const IdComponent>().each()) {
    if (team.id == rampage::TeamId::kEnemy) enemies.push_back(id.guid);
  }
  std::sort(enemies.begin(), enemies.end());
  sink.BeginCoalesce("Remove enemies");
  for (const Guid& guid : enemies) {
    sink.Submit(std::make_unique<DestroyEntityCommand>(guid));
  }
  sink.EndCoalesce();
  SE_LOG_INFO("stress generator: removed {} enemies", enemies.size());
}

void StressPanel::Draw(const PanelContext& context) {
  if (context.world == nullptr) {
    ImGui::TextDisabled("No document");
    return;
  }
  EditSink sink(context);
  ImGui::BeginDisabled(!sink.CanEdit());
  ImGui::SliderInt("Entity Amount", &amount_, 0, 1000);
  ProbeItem("stress.amount");
  ImGui::SliderInt("Spawns per tick", &per_tick_, 1, 512);
  ProbeItem("stress.per_tick");
  const float width = (ImGui::GetContentRegionAvail().x - 16.0F) / 3.0F;
  if (ImGui::Button("Replace Entity", {width, 0.0F})) {
    RemoveEnemies(context, sink);
    Generate(context, sink);
  }
  ProbeItem("stress.replace");
  ImGui::SameLine();
  if (ImGui::Button("Generate Entity", {width, 0.0F})) Generate(context, sink);
  ProbeItem("stress.generate");
  ImGui::SameLine();
  if (ImGui::Button("Remove Entity", {width, 0.0F})) {
    RemoveEnemies(context, sink);
  }
  ProbeItem("stress.remove");
  ImGui::EndDisabled();
  if (sink.Playing()) {
    const std::string next =
        std::format("Spawns spread over ticks from tick {}", sink.NextTick());
    ImGui::TextDisabled("%s", next.c_str());
  }
}

}  // namespace se::editor
