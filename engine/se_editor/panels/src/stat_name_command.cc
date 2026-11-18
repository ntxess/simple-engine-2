// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor_panels/stat_name_command.h"

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

#include "game_rampage/components.h"
#include "game_rampage/scene.h"
#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor_panels/panel_support.h"

namespace se::editor {

StatNameCommand::StatNameCommand(std::string name)
    : name_(std::move(name)),
      id_(rampage::StatIdOf(name_)),
      label_("Name stat " + name_) {}

std::expected<void, Error> StatNameCommand::Apply(World& world) {
  if (name_.empty()) {
    return Fail(ErrorCode::kInvalidArgument, "a stat needs a name");
  }
  entt::registry& reg = world.Reg();
  auto* names = reg.ctx().find<rampage::StatNames>();
  if (names == nullptr) names = &reg.ctx().emplace<rampage::StatNames>();
  const std::string_view known = names->Name(id_);
  if (known.empty()) {
    names->Add(name_);
    added_ = true;
    return {};
  }
  if (known != name_) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("the stat '{}' has the id of '{}' ({})", name_,
                            known, id_));
  }
  added_ = false;
  return {};
}

std::expected<void, Error> StatNameCommand::Revert(World& world) {
  if (!added_) return {};
  auto* names = world.Reg().ctx().find<rampage::StatNames>();
  if (names != nullptr) names->Remove(id_);
  added_ = false;
  return {};
}

std::expected<std::vector<SessionEntry>, Error> StatNameCommand::ToSession(
    const ComponentRegistry& /*registry*/, Tick /*apply_before*/) const {
  return std::vector<SessionEntry>{};
}

namespace {

// Records each of |names| |table| lacks into |added|; a name whose id
// another name has is an error.
template <typename Table>
std::expected<void, Error> AddNames(Table& table,
                                    const std::vector<std::string>& names,
                                    std::vector<std::uint32_t>& added,
                                    std::string_view what) {
  for (const std::string& name : names) {
    const std::uint32_t id = Fnv1a32(name);
    const std::string_view known = table.Name(id);
    if (known.empty()) {
      table.Add(name);
      added.push_back(id);
    } else if (known != name) {
      return Fail(ErrorCode::kInvalidArgument,
                  std::format("the {} '{}' has the id of '{}' ({})", what, name,
                              known, id));
    }
  }
  return {};
}

template <typename Table>
void RemoveNames(Table* table, std::vector<std::uint32_t>& added) {
  if (table != nullptr) {
    for (const std::uint32_t id : added) table->Remove(id);
  }
  added.clear();
}

template <typename Table>
Table& TableOf(entt::registry& reg) {
  auto* table = reg.ctx().find<Table>();
  return table != nullptr ? *table : reg.ctx().emplace<Table>();
}

template <typename Table>
std::vector<std::string> Missing(const World& world,
                                 const std::vector<std::string>& names) {
  const auto* table = world.CReg().ctx().find<Table>();
  std::vector<std::string> out;
  for (const std::string& name : names) {
    if (table == nullptr || table->Name(Fnv1a32(name)) != name) {
      out.push_back(name);
    }
  }
  return out;
}

}  // namespace

PrefabNamesCommand::PrefabNamesCommand(std::string label,
                                       std::vector<std::string> stats,
                                       std::vector<std::string> textures)
    : label_(std::move(label)),
      stats_(std::move(stats)),
      textures_(std::move(textures)) {}

std::expected<void, Error> PrefabNamesCommand::Apply(World& world) {
  entt::registry& reg = world.Reg();
  added_stats_.clear();
  added_textures_.clear();
  auto stats =
      AddNames(TableOf<rampage::StatNames>(reg), stats_, added_stats_, "stat");
  if (stats) {
    stats = AddNames(TableOf<rampage::TextureNames>(reg), textures_,
                     added_textures_, "texture");
  }
  if (!stats) {
    // Nothing stays of a failed Apply.
    RemoveNames(reg.ctx().find<rampage::StatNames>(), added_stats_);
    RemoveNames(reg.ctx().find<rampage::TextureNames>(), added_textures_);
    return stats;
  }
  return {};
}

std::expected<void, Error> PrefabNamesCommand::Revert(World& world) {
  entt::registry& reg = world.Reg();
  RemoveNames(reg.ctx().find<rampage::StatNames>(), added_stats_);
  RemoveNames(reg.ctx().find<rampage::TextureNames>(), added_textures_);
  return {};
}

std::expected<std::vector<SessionEntry>, Error> PrefabNamesCommand::ToSession(
    const ComponentRegistry& /*registry*/, Tick /*apply_before*/) const {
  return std::vector<SessionEntry>{};
}

std::expected<std::unique_ptr<EditorCommand>, Error> MakePrefabNames(
    const World& world, std::string_view ref, std::string_view text) {
  auto names = rampage::PrefabNames(text, ref);
  if (!names) return std::unexpected(std::move(names.error()));
  std::vector<std::string> stats =
      Missing<rampage::StatNames>(world, names->stats);
  std::vector<std::string> textures =
      Missing<rampage::TextureNames>(world, names->textures);
  if (stats.empty() && textures.empty()) {
    return std::unique_ptr<EditorCommand>{};
  }
  return std::unique_ptr<EditorCommand>(std::make_unique<PrefabNamesCommand>(
      std::format("Names of {}", ref), std::move(stats), std::move(textures)));
}

bool HasStatName(const World& world, std::string_view name) {
  const auto* names = world.CReg().ctx().find<rampage::StatNames>();
  return names != nullptr && names->Name(rampage::StatIdOf(name)) == name &&
         !name.empty();
}

std::unique_ptr<EditorCommand> WithStatNames(
    std::string label, const std::vector<std::string>& names,
    std::unique_ptr<EditorCommand> set) {
  auto group = std::make_unique<CommandGroup>(std::move(label));
  for (const std::string& name : names) {
    group->Add(std::make_unique<StatNameCommand>(name));
  }
  group->Add(std::move(set));
  return group;
}

std::expected<std::unique_ptr<EditorCommand>, Error> MakeStatNameEdit(
    const World& world, Guid guid, std::string_view name, float value) {
  if (name.empty()) {
    return Fail(ErrorCode::kInvalidArgument, "a stat needs a name");
  }
  const std::optional<entt::entity> entity = world.Find(guid);
  if (!entity) {
    return Fail(ErrorCode::kNotFound,
                std::format("no entity {} in the World", ToString(guid)));
  }
  rampage::Stats stats;
  std::string before;
  if (const auto* current = world.CReg().try_get<rampage::Stats>(*entity)) {
    stats = *current;
    auto json = ReadComponentJson(world, *entity, "Stats");
    if (!json) return std::unexpected(std::move(json.error()));
    before = std::move(*json);
  }
  rampage::SetStat(stats, rampage::StatIdOf(name), value);
  auto after = JsonOfValue(world, "Stats", stats);
  if (!after) return std::unexpected(std::move(after.error()));
  auto set = std::make_unique<SetComponentCommand>(
      guid, "Stats", std::move(before), std::move(*after));
  return WithStatNames(std::format("Add stat {}", name), {std::string(name)},
                       std::move(set));
}

}  // namespace se::editor
