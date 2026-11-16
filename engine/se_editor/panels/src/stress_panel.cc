// Copyright 2026 The simple-engine-2 Authors.
//
// The Stress generator panel: the legacy Debug panel's entity controls
// (app/editor/Editor.cpp:278-333 at master): an amount slider and Replace,
// Generate and Remove. Remove destroys every entity on the enemy team (the
// legacy buttons destroyed every ENEMY TeamTag). A press is planned at once
// (stress_generator.h) and spread: in Play, its batches are queued in the
// SessionLog against consecutive ticks; in Edit mode one batch of
// create-entity commands is submitted per frame, all in one undo step.

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

std::size_t StressPanel::Pending() const noexcept {
  if (!job_) return 0;
  std::size_t pending = 0;
  for (std::size_t b = job_->next; b < job_->batches.size(); ++b) {
    pending += job_->batches[b].count;
  }
  return pending;
}

void StressPanel::SubmitBatch(EditSink& sink, Job& job, std::size_t batch,
                              Tick apply_before) {
  const SpawnBatch& b = job.batches[batch];
  for (std::uint32_t i = b.first; i < b.first + b.count; ++i) {
    const StressSpawn& spawn = job.spawns[i];
    sink.Submit(std::make_unique<CreateEntityCommand>(
                    spawn.guid, ToCommandComponents(spawn)),
                apply_before);
  }
}

void StressPanel::Generate(const PanelContext& context, EditSink& sink) {
  const World& world = *context.world;
  const std::uint64_t seed = world.Desc().seed;
  // Presses already in the World (an earlier session's, saved) are skipped,
  // so their Guids never collide.
  while (world.Find(StressGuid(seed, press_, 0)).has_value()) ++press_;
  float width = 1920.0F;
  float height = 1080.0F;
  if (const auto* bounds = world.CReg().ctx().find<rampage::ArenaBounds>()) {
    width = bounds->w;
    height = bounds->h;
  }
  const StressSpec spec = DefaultStressSpec(width, height);
  const auto count = static_cast<std::uint32_t>(std::max(amount_, 0));
  Job job;
  job.spawns = PlanStress(spec, seed, press_, count);
  job.batches =
      SpreadSpawns(count, static_cast<std::uint32_t>(std::max(per_tick_, 1)));
  job.label = std::format("Generate {} enemies", count);
  const std::uint32_t press = press_++;
  if (job.batches.empty()) return;
  if (sink.Playing()) {
    const Tick first = sink.NextTick();
    for (std::size_t b = 0; b < job.batches.size(); ++b) {
      SubmitBatch(sink, job, b, first + job.batches[b].offset);
    }
    SE_LOG_INFO(
        "stress generator: press {}: {} enemies over {} ticks from tick {}",
        press, count, job.batches.size(), first);
    return;
  }
  SE_LOG_INFO("stress generator: press {}: {} enemies over {} frames", press,
              count, job.batches.size());
  sink.BeginCoalesce(job.label);
  SubmitBatch(sink, job, 0, 0);
  job.next = 1;
  if (job.next < job.batches.size()) {
    job_ = std::move(job);
  } else {
    sink.EndCoalesce();
  }
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
  // A press in progress (Edit mode): the next batch, this frame.
  if (job_.has_value()) {
    Job& job = job_.value();
    if (sink.CanEdit() && !sink.Playing()) {
      SubmitBatch(sink, job, job.next, 0);
      ++job.next;
    }
    if (!sink.CanEdit() || sink.Playing() || job.next >= job.batches.size()) {
      sink.EndCoalesce();
      job_.reset();
    }
  }

  ImGui::BeginDisabled(!sink.CanEdit() || job_.has_value());
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
  if (job_) {
    ImGui::Text("Spawning: %zu left", Pending());
  } else if (sink.Playing()) {
    const std::string next =
        std::format("Spawns spread over ticks from tick {}", sink.NextTick());
    ImGui::TextDisabled("%s", next.c_str());
  }
}

}  // namespace se::editor
