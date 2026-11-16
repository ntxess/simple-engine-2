// Copyright 2026 The simple-engine-2 Authors.
//
// The Hierarchy panel. The engine has no parent relation yet (prefabs and
// nesting arrive with M11), so the parent column shows none and the list is
// flat, in spawn order (ADR-100).

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/panel.h"
#include "se_editor/selection.h"
#include "se_editor_panels/panel_support.h"
#include "se_editor_panels/panels.h"

namespace se::editor {
namespace {

struct Row {
  std::uint32_t serial = 0;
  Guid guid;
  entt::entity entity = entt::null;
};

bool Matches(std::string_view filter, std::string_view name,
             const std::string& guid) {
  if (filter.empty()) return true;
  return name.find(filter) != std::string_view::npos ||
         guid.find(filter) != std::string::npos;
}

}  // namespace

Guid NewEntityGuid(const World& world, std::string_view purpose) {
  Hasher base(0x45444954454E5459ULL);  // "EDITENTY"
  base.AddString(world.Desc().name);
  base.AddString(purpose);
  for (std::uint64_t n = world.EntityCount();; ++n) {
    Hasher hasher = base;
    hasher.AddU64(n);
    Guid guid;
    guid.hi = hasher.Finish();
    hasher.AddU64(1);
    guid.lo = hasher.Finish();
    if (!guid.IsNil() && !world.Find(guid).has_value()) return guid;
  }
}

void HierarchyPanel::Draw(const PanelContext& context) {
  rows_.clear();
  if (context.world == nullptr) {
    ImGui::TextDisabled("No document");
    return;
  }
  const World& world = *context.world;
  EditSink sink(context);

  ImGui::SetNextItemWidth(-1.0F);
  ImGui::InputTextWithHint("##filter", "Filter by name or Guid", &filter_);
  ProbeItem("hierarchy.filter");

  ImGui::BeginDisabled(!sink.CanEdit());
  if (ImGui::Button("Create entity")) {
    std::vector<ComponentJson> components{{"Name", R"({"value":"Entity"})"},
                                          {"Transform", "{}"}};
    sink.Submit(std::make_unique<CreateEntityCommand>(
        NewEntityGuid(world, "entity"), std::move(components)));
  }
  ProbeItem("hierarchy.create");
  ImGui::SameLine();
  const bool any_selected =
      context.selection != nullptr && !context.selection->Empty();
  ImGui::BeginDisabled(!any_selected);
  if (ImGui::Button("Destroy selected") && any_selected) {
    sink.BeginCoalesce("Destroy entities");
    for (const Guid& guid : context.selection->Guids()) {
      sink.Submit(std::make_unique<DestroyEntityCommand>(guid));
    }
    sink.EndCoalesce();
  }
  ProbeItem("hierarchy.destroy");
  ImGui::EndDisabled();
  ImGui::EndDisabled();

  std::vector<Row> rows;
  const entt::registry& reg = world.CReg();
  for (const auto [e, id] : reg.view<const IdComponent>().each()) {
    const auto* spawned = reg.try_get<Spawned>(e);
    rows.push_back({spawned != nullptr
                        ? spawned->spawn_serial
                        : std::numeric_limits<std::uint32_t>::max(),
                    id.guid, e});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return a.serial != b.serial ? a.serial < b.serial : a.guid < b.guid;
  });

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_Resizable;
  if (!ImGui::BeginTable("##entities", 3, kFlags)) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Name");
  ImGui::TableSetupColumn("Guid");
  ImGui::TableSetupColumn("Parent");
  ImGui::TableHeadersRow();
  for (const Row& row : rows) {
    const std::string_view name = EntityName(world, row.entity);
    const std::string guid = ToString(row.guid);
    if (!Matches(filter_, name, guid)) continue;
    rows_.push_back(row.guid);
    const bool selected =
        context.selection != nullptr && context.selection->Contains(row.guid);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(guid.c_str());
    const std::string label =
        name.empty() ? std::string("(unnamed)") : std::string(name);
    if (ImGui::Selectable(label.c_str(), selected,
                          ImGuiSelectableFlags_SpanAllColumns)) {
      if (ImGui::GetIO().KeyCtrl && context.selection != nullptr) {
        std::vector<Guid> next(context.selection->Guids().begin(),
                               context.selection->Guids().end());
        const auto it =
            std::find_if(next.begin(), next.end(),
                         [&row](const Guid& guid) { return guid == row.guid; });
        if (it != next.end()) {
          next.erase(it);
        } else {
          next.push_back(row.guid);
        }
        sink.Select(next);
      } else {
        sink.Select(std::span<const Guid>(&row.guid, 1));
      }
    }
    ProbeItem("hierarchy.row." + guid);
    ImGui::PopID();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(ShortGuid(row.guid).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", guid.c_str());
    ImGui::TableNextColumn();
    ImGui::TextDisabled("-");
  }
  ImGui::EndTable();
}

}  // namespace se::editor
