// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor_panels/stat_name_command.h"

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
#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/guid.h"
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
