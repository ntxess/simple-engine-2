// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor_panels/panel_support.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <entt/entity/registry.hpp>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/log.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/panel.h"
#include "se_editor/prefab_commands.h"

namespace se::editor {

bool EditSink::CanEdit() const noexcept {
  if (context_->faulted) return false;
  return context_->playing ? context_->play_edits != nullptr
                           : context_->commands != nullptr;
}

bool EditSink::CanSelect() const noexcept {
  return context_->commands != nullptr || context_->play_edits != nullptr;
}

bool EditSink::Playing() const noexcept { return context_->playing; }

Tick EditSink::NextTick() const noexcept {
  return context_->play_edits != nullptr ? context_->play_edits->NextTick() : 0;
}

bool EditSink::Submit(std::unique_ptr<EditorCommand> command,
                      Tick apply_before) {
  if (command == nullptr || !CanEdit()) return false;
  if (context_->playing) {
    context_->play_edits->Submit(std::move(command), apply_before);
  } else {
    context_->commands->Submit(std::move(command));
  }
  return true;
}

void EditSink::BeginCoalesce(std::string_view label) {
  if (CanEdit() && !context_->playing) context_->commands->BeginCoalesce(label);
}

void EditSink::EndCoalesce() {
  if (CanEdit() && !context_->playing) context_->commands->EndCoalesce();
}

bool EditSink::Select(std::span<const Guid> guids) {
  if (context_->commands != nullptr) {
    context_->commands->Select(guids);
    return true;
  }
  if (context_->play_edits != nullptr) {
    context_->play_edits->Select(guids);
    return true;
  }
  return false;
}

void EditSink::ReportError(const Error& error) {
  SE_LOG_WARN("editor: an edit was dropped: {}", error.message);
}

void TrackCoalesce(EditSink& sink, std::string_view label) {
  if (ImGui::IsItemActivated()) sink.BeginCoalesce(label);
  if (ImGui::IsItemDeactivated()) sink.EndCoalesce();
}

const ComponentInfo* FindInfo(const World& world,
                              std::string_view name) noexcept {
  return world.Registry().FindByName(name);
}

bool HasComponent(const World& world, entt::entity entity,
                  const ComponentInfo& info) noexcept {
  const entt::sparse_set* storage = world.CReg().storage(info.id);
  return storage != nullptr && storage->contains(entity);
}

std::expected<std::string, Error> ReadComponentJson(const World& world,
                                                    entt::entity entity,
                                                    std::string_view name) {
  const ComponentInfo* info = FindInfo(world, name);
  if (info == nullptr || info->write_json == nullptr) {
    return Fail(ErrorCode::kNotFound, "no such component");
  }
  return info->write_json(world.CReg(), entity);
}

void WidgetProbe::Record(std::string key, ImVec2 min, ImVec2 max) {
  const Rect rect{min.x, min.y, max.x - min.x, max.y - min.y};
  for (auto& [name, r] : rects_) {
    if (name == key) {
      r = rect;
      return;
    }
  }
  rects_.emplace_back(std::move(key), rect);
}

void WidgetProbe::RecordLastItem(std::string key) {
  Record(std::move(key), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

std::optional<Rect> WidgetProbe::Find(std::string_view key) const {
  for (const auto& [name, rect] : rects_) {
    if (name == key) return rect;
  }
  return std::nullopt;
}

std::optional<ImVec2> WidgetProbe::Center(std::string_view key) const {
  const std::optional<Rect> rect = Find(key);
  if (!rect) return std::nullopt;
  return ImVec2(rect->x + rect->w / 2.0F, rect->y + rect->h / 2.0F);
}

void Probed::ProbeItem(std::string_view key) const {
  if (probe_ != nullptr) probe_->RecordLastItem(std::string(key));
}

void Probed::ProbeRect(std::string_view key, ImVec2 min, ImVec2 max) const {
  if (probe_ != nullptr) probe_->Record(std::string(key), min, max);
}

std::optional<std::string> TextEdits::Input(const char* label,
                                            const std::string& current,
                                            float width) {
  const ImGuiID id = ImGui::GetID(label);
  auto it = buffers_.find(id);
  if (it == buffers_.end()) it = buffers_.emplace(id, current).first;
  if (width != 0.0F) ImGui::SetNextItemWidth(width);
  ImGui::InputText(label, &it->second);
  std::optional<std::string> committed;
  if (ImGui::IsItemDeactivatedAfterEdit() && it->second != current) {
    committed = it->second;
  }
  if (!ImGui::IsItemActive()) buffers_.erase(id);
  return committed;
}

std::string ShortGuid(const Guid& guid) { return ToString(guid).substr(0, 8); }

std::string_view EntityName(const World& world, entt::entity entity) {
  const auto* name = world.CReg().try_get<Name>(entity);
  return name != nullptr ? std::string_view(name->value) : std::string_view{};
}

const EntityJson* PrefabBaselines::Get(const PanelContext& context, Guid guid) {
  if (context.world == nullptr || context.game == nullptr ||
      !context.game->prefab_baseline) {
    return nullptr;
  }
  const std::optional<std::string> ref = PrefabOf(*context.world, guid);
  if (!ref) return nullptr;
  if (world_ != context.world || document_ != context.document) {
    by_ref_.clear();
    world_ = context.world;
    document_ = context.document;
  }
  std::optional<std::string> text;
  if (context.prefab_texts != nullptr) {
    if (const auto it = context.prefab_texts->find(*ref);
        it != context.prefab_texts->end()) {
      text = it->second;
    }
  }
  const auto found = by_ref_.find(*ref);
  if (found != by_ref_.end() && found->second.text == text &&
      (text.has_value() || found->second.revision == context.revision)) {
    return found->second.baseline ? &*found->second.baseline : nullptr;
  }
  ++loads_;
  auto baseline = context.game->prefab_baseline(*context.world, guid,
                                                std::string(context.asset_root),
                                                context.prefab_texts);
  Entry entry;
  entry.text = std::move(text);
  entry.revision = context.revision;
  if (baseline) {
    entry.baseline = std::move(*baseline);
  } else {
    EditSink::ReportError(baseline.error());
  }
  Entry& kept = by_ref_.insert_or_assign(*ref, std::move(entry)).first->second;
  return kept.baseline ? &*kept.baseline : nullptr;
}

}  // namespace se::editor
