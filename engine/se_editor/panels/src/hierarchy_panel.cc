// Copyright 2026 The simple-engine-2 Authors.
//
// The Hierarchy panel. The engine has no parent relation (prefabs are not
// nested, M11), so the parent column shows none and the list is flat, in
// spawn order (ADR-100). The prefab column shows each instance's prefab
// (its file name; "-" for an entity without one) and marks an instance
// with overrides ("*"): what differs from a bare instance of its prefab
// (EditorGame::prefab_baseline), computed only for the rows on screen and
// kept until the document changes (Edit mode; ADR-119). A prefab
// document's one root has no prefab column.

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <span>
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
#include "se_editor/prefab_commands.h"
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

std::string HierarchyPanel::PrefabCell(const Guid& guid) const {
  const auto it = cells_.find(guid);
  return it != cells_.end() ? it->second : std::string();
}

bool HierarchyPanel::HasOverrides(const PanelContext& context,
                                  const Guid& guid) {
  if (context.playing || context.faulted || context.game == nullptr ||
      !context.game->prefab_baseline) {
    return false;
  }
  if (cache_world_ != context.world || cache_document_ != context.document ||
      cache_revision_ != context.revision) {
    overrides_.clear();
    cache_world_ = context.world;
    cache_document_ = context.document;
    cache_revision_ = context.revision;
  }
  if (const auto it = overrides_.find(guid); it != overrides_.end()) {
    return it->second;
  }
  auto baseline = context.game->prefab_baseline(
      *context.world, guid, std::string(context.asset_root));
  const bool has =
      baseline.has_value() &&
      !OverriddenComponents(*context.world, guid, *baseline).empty();
  overrides_.emplace(guid, has);
  return has;
}

void HierarchyPanel::DrawRow(const PanelContext& context, EditSink& sink,
                             const Guid& row, entt::entity entity,
                             bool prefab_column) {
  const std::string_view name = EntityName(*context.world, entity);
  const std::string guid = ToString(row);
  const bool selected =
      context.selection != nullptr && context.selection->Contains(row);
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::PushID(guid.c_str());
  const std::string label =
      name.empty() ? std::string("(unnamed)") : std::string(name);
  if (ImGui::Selectable(label.c_str(), selected,
                        ImGuiSelectableFlags_SpanAllColumns)) {
    if (ImGui::GetIO().KeyCtrl && context.selection != nullptr) {
      // Ctrl-click toggles the row in the selection.
      std::vector<Guid> next(context.selection->Guids().begin(),
                             context.selection->Guids().end());
      const auto it =
          std::find_if(next.begin(), next.end(),
                       [&row](const Guid& guid) { return guid == row; });
      if (it != next.end()) {
        next.erase(it);
      } else {
        next.push_back(row);
      }
      sink.Select(next);
    } else {
      sink.Select(std::span<const Guid>(&row, 1));
    }
  }
  ProbeItem("hierarchy.row." + guid);
  ImGui::PopID();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(ShortGuid(row).c_str());
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", guid.c_str());
  if (prefab_column) {
    ImGui::TableNextColumn();
    const std::optional<std::string> prefab = PrefabOf(*context.world, row);
    std::string cell = "-";
    if (prefab) {
      cell = prefab->substr(prefab->find_last_of('/') + 1);
      if (HasOverrides(context, row)) cell += " *";
    }
    ImGui::TextUnformatted(cell.c_str());
    ProbeItem("hierarchy.prefab." + guid);
    if (prefab && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s%s", prefab->c_str(),
                        cell.ends_with(" *") ? " (with overrides)" : "");
    }
    cells_[row] = std::move(cell);
  }
  ImGui::TableNextColumn();
  ImGui::TextDisabled("-");
}

void HierarchyPanel::Draw(const PanelContext& context) {
  rows_.clear();
  cells_.clear();
  drawn_ = 0;
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

  // The rows the filter keeps; only the ones on screen are drawn (a stress
  // press makes thousands).
  if (!filter_.empty()) {
    std::erase_if(rows, [this, &world](const Row& row) {
      return !Matches(filter_, EntityName(world, row.entity),
                      ToString(row.guid));
    });
  }
  rows_.reserve(rows.size());
  for (const Row& row : rows) rows_.push_back(row.guid);

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_Resizable;
  // A prefab document's root is the prefab: no prefab column.
  const bool prefab_column = !context.prefab_document;
  if (!ImGui::BeginTable("##entities", prefab_column ? 4 : 3, kFlags)) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  // The name takes the most room; the parent column (always "-": there
  // is no parent relation) the least.
  constexpr ImGuiTableColumnFlags kStretch = ImGuiTableColumnFlags_WidthStretch;
  ImGui::TableSetupColumn("Name", kStretch, 2.0F);
  ImGui::TableSetupColumn("Guid", kStretch, 1.0F);
  if (prefab_column) ImGui::TableSetupColumn("Prefab", kStretch, 1.5F);
  ImGui::TableSetupColumn("Parent", kStretch, 0.5F);
  ImGui::TableHeadersRow();
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(rows.size()));
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      DrawRow(context, sink, rows[static_cast<std::size_t>(i)].guid,
              rows[static_cast<std::size_t>(i)].entity, prefab_column);
      ++drawn_;
    }
  }
  ImGui::EndTable();
}

}  // namespace se::editor
