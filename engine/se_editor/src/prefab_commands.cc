// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor/prefab_commands.h"

#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/edit_document.h"
#include "se_editor/editor_game.h"
#include "src/json_value.h"

namespace se::editor {
namespace {

constexpr std::string_view kReserved[] = {"IdComponent", "Name", "Spawned",
                                          kPrefabInstanceComponent,
                                          "DestroyRequest"};

const ComponentJson* FindIn(const std::vector<ComponentJson>& components,
                            std::string_view name) {
  for (const ComponentJson& component : components) {
    if (component.component == name) return &component;
  }
  return nullptr;
}

std::optional<std::string> CurrentJson(const World& world, Guid guid,
                                       std::string_view component) {
  const std::optional<entt::entity> entity = world.Find(guid);
  const ComponentInfo* info = world.Registry().FindByName(component);
  if (!entity || info == nullptr || info->write_json == nullptr) {
    return std::nullopt;
  }
  const entt::sparse_set* storage = world.CReg().storage(info->id);
  if (storage == nullptr || !storage->contains(*entity)) return std::nullopt;
  auto json = info->write_json(world.CReg(), *entity);
  if (!json) return std::nullopt;
  return std::move(*json);
}

bool Serialized(const World& world, std::string_view component) {
  const ComponentInfo* info = world.Registry().FindByName(component);
  return info != nullptr && HasFlag(info->flags, ComponentFlags::kSerialized);
}

}  // namespace

bool PrefabReserved(std::string_view component) noexcept {
  for (const std::string_view reserved : kReserved) {
    if (component == reserved) return true;
  }
  return false;
}

std::optional<std::string> PrefabOf(const World& world, Guid guid) {
  const std::optional<std::string> json =
      CurrentJson(world, guid, kPrefabInstanceComponent);
  if (!json) return std::nullopt;
  auto value = json::Parse(*json);
  if (!value) return std::nullopt;
  const json::Value* prefab = value->Find("prefab");
  if (prefab == nullptr || prefab->kind != json::Kind::kString ||
      prefab->text.empty()) {
    return std::nullopt;
  }
  return prefab->text;
}

bool HoldsComponent(const World& world, Guid guid, std::string_view component) {
  const std::optional<entt::entity> entity = world.Find(guid);
  const ComponentInfo* info = world.Registry().FindByName(component);
  if (!entity || info == nullptr) return false;
  const entt::sparse_set* storage = world.CReg().storage(info->id);
  return storage != nullptr && storage->contains(*entity);
}

bool RemovableBy(const EditorGame& game, std::string_view component) {
  return !game.removable || game.removable(component);
}

std::expected<std::unique_ptr<EditorCommand>, Error> MakeRemoval(
    const World& world, Guid guid, std::string_view component,
    const EditorGame& game) {
  if (!RemovableBy(game, component)) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("'{}' may not be removed (the entity's identity, "
                            "its prefab link or its place)",
                            component));
  }
  if (!world.Find(guid)) {
    return Fail(ErrorCode::kNotFound,
                std::format("no entity {} in the World", ToString(guid)));
  }
  if (!HoldsComponent(world, guid, component)) {
    return Fail(ErrorCode::kNotFound,
                std::format("the entity has no '{}'", component));
  }
  auto group =
      std::make_unique<CommandGroup>(std::format("Remove {}", component));
  group->Add(
      std::make_unique<RemoveComponentCommand>(guid, std::string(component)));
  if (game.removed_with) {
    for (const std::string& derived : game.removed_with(component)) {
      if (derived != component && HoldsComponent(world, guid, derived)) {
        group->Add(std::make_unique<RemoveComponentCommand>(guid, derived));
      }
    }
  }
  return std::unique_ptr<EditorCommand>(std::move(group));
}

std::optional<std::string> BaselineJson(const EntityJson& baseline,
                                        std::string_view component) {
  const ComponentJson* found = FindIn(baseline.components, component);
  if (found == nullptr) return std::nullopt;
  return found->json;
}

std::expected<std::unique_ptr<EditorCommand>, Error> MakeRevertToPrefab(
    const World& world, Guid guid, std::string_view component,
    const EntityJson& baseline) {
  if (PrefabReserved(component)) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("'{}' is not taken from the prefab", component));
  }
  if (!world.Find(guid)) {
    return Fail(ErrorCode::kNotFound,
                std::format("no entity {} in the World", ToString(guid)));
  }
  const std::optional<std::string> current =
      CurrentJson(world, guid, component);
  const std::optional<std::string> base = BaselineJson(baseline, component);
  if (current == base) return std::unique_ptr<EditorCommand>{};
  if (!base) {
    return std::unique_ptr<EditorCommand>(
        std::make_unique<RemoveComponentCommand>(guid, std::string(component)));
  }
  auto set = std::make_unique<SetComponentCommand>(
      guid, std::string(component), current.value_or(std::string()), *base);
  return std::unique_ptr<EditorCommand>(std::move(set));
}

bool DiffersFromBaseline(const World& world, Guid guid,
                         std::string_view component,
                         const EntityJson& baseline) {
  return CurrentJson(world, guid, component) !=
         BaselineJson(baseline, component);
}

std::vector<std::string> OverriddenComponents(const World& world, Guid guid,
                                              const EntityJson& baseline) {
  std::vector<std::string> overridden;
  for (const ComponentInfo& info : world.Registry().All()) {
    if (PrefabReserved(info.name) ||
        !HasFlag(info.flags, ComponentFlags::kEditorVisible) ||
        !HasFlag(info.flags, ComponentFlags::kSerialized)) {
      continue;
    }
    if (DiffersFromBaseline(world, guid, info.name, baseline)) {
      overridden.emplace_back(info.name);
    }
  }
  return overridden;
}

std::expected<std::unique_ptr<EditorCommand>, Error> MakeRebase(
    const World& world, std::string label,
    const std::vector<EntityJson>& results) {
  auto group = std::make_unique<CommandGroup>(std::move(label));
  for (const EntityJson& result : results) {
    auto current = SerializeEntity(world, result.guid);
    if (!current) return std::unexpected(std::move(current.error()));
    for (const ComponentJson& wanted : result.components) {
      if (PrefabReserved(wanted.component)) continue;
      const ComponentJson* held = FindIn(*current, wanted.component);
      if (held != nullptr && held->json == wanted.json) continue;
      group->Add(std::make_unique<SetComponentCommand>(
          result.guid, wanted.component,
          held != nullptr ? held->json : std::string(), wanted.json));
    }
    for (const ComponentJson& held : *current) {
      if (PrefabReserved(held.component) ||
          FindIn(result.components, held.component) != nullptr) {
        continue;
      }
      // Only what a scene says goes: state the loader derives without
      // serializing it (a previous pose, a path's cache) stays.
      if (!Serialized(world, held.component)) continue;
      group->Add(std::make_unique<RemoveComponentCommand>(result.guid,
                                                          held.component));
    }
  }
  if (group->empty()) return std::unique_ptr<EditorCommand>{};
  return std::unique_ptr<EditorCommand>(std::move(group));
}

std::expected<bool, Error> RebaseDocument(EditDocument& document,
                                          const EditorGame& game,
                                          std::string_view ref,
                                          const std::string& new_text) {
  if (document.Desc().kind != DocumentKind::kScene) return false;
  const auto recorded = document.PrefabTexts().find(ref);
  if (recorded == document.PrefabTexts().end()) return false;
  if (recorded->second == new_text) return false;
  if (!game.rebase_instances) {
    document.SetPrefabText(ref, new_text);
    return false;
  }
  auto results = game.rebase_instances(document.Get(), ref, recorded->second,
                                       new_text, document.Desc().asset_root);
  if (!results) return std::unexpected(std::move(results.error()));
  auto command = MakeRebase(document.Get(),
                            std::format("Prefab changed: {}", ref), *results);
  if (!command) return std::unexpected(std::move(command.error()));
  // Not part of undo: after an undo the instances differ from the file's
  // prefab, which the next save writes as overrides.
  document.SetPrefabText(ref, new_text);
  if (*command == nullptr) return false;
  if (auto applied = document.ApplyPrefabChange(std::move(*command));
      !applied) {
    return std::unexpected(std::move(applied.error()));
  }
  return true;
}

FileTextAction ClassifyFileText(const EditDocument& document,
                                std::string_view text) noexcept {
  if (text == document.FileText()) return FileTextAction::kUnchanged;
  return document.Dirty() ? FileTextAction::kAsk : FileTextAction::kReload;
}

std::expected<std::unique_ptr<EditDocument>, Error> ReopenDocument(
    const EditDocument& document, const EditorGame& game,
    std::string_view text) {
  return EditDocument::Open(document.Get().Registry(), game, document.Desc(),
                            text);
}

}  // namespace se::editor
