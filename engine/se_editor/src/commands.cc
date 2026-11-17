// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor/commands.h"

#include <cstddef>
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
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_sim/session_log.h"
#include "src/json_value.h"

namespace se::editor {
namespace {

// Components an entity bundle never carries: the Guid comes from Create,
// and a destroy request is not part of the entity.
constexpr std::string_view kIdComponent = "IdComponent";
constexpr std::string_view kDestroyRequest = "DestroyRequest";

std::expected<entt::entity, Error> FindEntity(const World& world, Guid guid) {
  const std::optional<entt::entity> entity = world.Find(guid);
  if (!entity) {
    return Fail(ErrorCode::kNotFound,
                std::format("no entity {} in the World", ToString(guid)));
  }
  return *entity;
}

std::expected<const ComponentInfo*, Error> FindComponent(
    const World& world, std::string_view name) {
  const ComponentInfo* info = world.Registry().FindByName(name);
  if (info == nullptr) {
    return Fail(ErrorCode::kNotFound,
                std::format("no component '{}' is registered", name));
  }
  if (info->read_json == nullptr || info->write_json == nullptr) {
    return Fail(ErrorCode::kUnsupported,
                std::format("component '{}' has no JSON hooks", name));
  }
  return info;
}

bool HasComponent(const World& world, const ComponentInfo& info,
                  entt::entity entity) {
  const entt::sparse_set* storage = world.CReg().storage(info.id);
  return storage != nullptr && storage->contains(entity);
}

std::expected<std::string, Error> ReadComponent(const World& world,
                                                const ComponentInfo& info,
                                                entt::entity entity) {
  if (!HasComponent(world, info, entity)) {
    return Fail(ErrorCode::kNotFound,
                std::format("the entity has no '{}'", info.name));
  }
  return info.write_json(world.CReg(), entity);
}

std::expected<void, Error> WriteComponent(World& world,
                                          const ComponentInfo& info,
                                          entt::entity entity,
                                          std::string_view json)
    SE_REQUIRES(g_sim_owner) {
  return info.read_json(world.Reg(), entity, json);
}

void RemoveComponent(World& world, const ComponentInfo& info,
                     entt::entity entity) SE_REQUIRES(g_sim_owner) {
  if (entt::sparse_set* storage = world.Reg().storage(info.id)) {
    if (storage->contains(entity)) storage->remove(entity);
  }
}

// Sets (or, with |json| empty, removes) one component of |target|.
std::expected<void, Error> SetOrRemove(World& world, Guid target,
                                       std::string_view component,
                                       std::string_view json)
    SE_REQUIRES(g_sim_owner) {
  auto entity = FindEntity(world, target);
  if (!entity) return std::unexpected(std::move(entity.error()));
  auto info = FindComponent(world, component);
  if (!info) return std::unexpected(std::move(info.error()));
  if (json.empty()) {
    RemoveComponent(world, **info, *entity);
    return {};
  }
  return WriteComponent(world, **info, *entity, json);
}

// Creates |target| with |components|; on a failure nothing is left.
std::expected<void, Error> CreateWith(
    World& world, Guid target, const std::vector<ComponentJson>& components)
    SE_REQUIRES(g_sim_owner) {
  if (target.IsNil()) {
    return Fail(ErrorCode::kInvalidArgument, "an entity needs a Guid");
  }
  if (world.Find(target)) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("entity {} already exists", ToString(target)));
  }
  std::vector<const ComponentInfo*> infos;
  infos.reserve(components.size());
  for (const ComponentJson& component : components) {
    auto info = FindComponent(world, component.component);
    if (!info) return std::unexpected(std::move(info.error()));
    infos.push_back(*info);
  }
  const entt::entity entity = world.Create(target);
  for (std::size_t i = 0; i < components.size(); ++i) {
    if (auto written =
            WriteComponent(world, *infos[i], entity, components[i].json);
        !written) {
      world.RequestDestroy(entity);
      world.DrainDestroyRequests();
      return written;
    }
  }
  return {};
}

std::expected<void, Error> DestroyGuid(World& world, Guid target)
    SE_REQUIRES(g_sim_owner) {
  auto entity = FindEntity(world, target);
  if (!entity) return std::unexpected(std::move(entity.error()));
  world.RequestDestroy(*entity);
  world.DrainDestroyRequests();
  return {};
}

std::vector<ComponentValue> AsValues(
    const std::vector<ComponentJson>& components) {
  std::vector<ComponentValue> values;
  values.reserve(components.size());
  for (const ComponentJson& component : components) {
    values.push_back(ComponentValue{component.component, component.json});
  }
  return values;
}

std::expected<std::vector<SessionEntry>, Error> One(
    std::expected<SessionEntry, Error> entry) {
  if (!entry) return std::unexpected(std::move(entry.error()));
  std::vector<SessionEntry> entries;
  entries.push_back(std::move(*entry));
  return entries;
}

std::expected<std::vector<Vec2>, Error> PointsOfValue(const json::Value& path) {
  const json::Value* points = path.Find("points");
  if (path.kind != json::Kind::kObject || points == nullptr ||
      points->kind != json::Kind::kArray) {
    return Fail(ErrorCode::kParse, "a path component needs a points array");
  }
  std::vector<Vec2> out;
  out.reserve(points->items.size());
  for (const json::Value& point : points->items) {
    const json::Value* x = point.Find("x");
    const json::Value* y = point.Find("y");
    const std::optional<double> px =
        x != nullptr ? x->AsDouble() : std::nullopt;
    const std::optional<double> py =
        y != nullptr ? y->AsDouble() : std::nullopt;
    if (!px || !py) {
      return Fail(ErrorCode::kParse, "a path point needs numbers x and y");
    }
    out.push_back(Vec2{static_cast<float>(*px), static_cast<float>(*py)});
  }
  return out;
}

json::Value PointValue(Vec2 point) {
  json::Value value = json::Value::Object();
  value.Set("x", json::Value::Float(point.x));
  value.Set("y", json::Value::Float(point.y));
  return value;
}

// A path whose point count changed restarts its progress when its segment
// no longer exists (ADR-096): the path systems index segments by it.
void ClampProgress(json::Value& path, std::size_t points) {
  json::Value* segment = path.Find("segment");
  if (segment == nullptr) return;
  const std::optional<double> current = segment->AsDouble();
  if (current && *current < static_cast<double>(points)) return;
  *segment = json::Value::Number(0.0);
  if (json::Value* u = path.Find("u")) *u = json::Value::Number(0.0);
  if (json::Value* done = path.Find("done")) {
    done->kind = json::Kind::kBool;
    done->boolean = false;
  }
}

}  // namespace

std::string_view CommandKindName(CommandKind kind) noexcept {
  switch (kind) {
    case CommandKind::kSetComponent:
      return "set-component";
    case CommandKind::kAddComponent:
      return "add-component";
    case CommandKind::kRemoveComponent:
      return "remove-component";
    case CommandKind::kCreateEntity:
      return "create-entity";
    case CommandKind::kDestroyEntity:
      return "destroy-entity";
    case CommandKind::kInsertPathPoint:
      return "insert-path-point";
    case CommandKind::kMovePathPoint:
      return "move-path-point";
    case CommandKind::kDeletePathPoint:
      return "delete-path-point";
    case CommandKind::kGroup:
      return "group";
    case CommandKind::kGame:
      return "game";
  }
  return "unknown";
}

std::expected<std::vector<ComponentJson>, Error> SerializeEntity(
    const World& world, Guid target) {
  auto entity = FindEntity(world, target);
  if (!entity) return std::unexpected(std::move(entity.error()));
  std::vector<ComponentJson> components;
  for (const ComponentInfo& info : world.Registry().All()) {
    if (info.name == kIdComponent || info.name == kDestroyRequest) continue;
    if (info.write_json == nullptr || info.read_json == nullptr) continue;
    if (!HasComponent(world, info, *entity)) continue;
    auto json = info.write_json(world.CReg(), *entity);
    if (!json) return std::unexpected(std::move(json.error()));
    components.push_back(ComponentJson{std::string(info.name), *json});
  }
  return components;
}

// --- SetComponentCommand ---

SetComponentCommand::SetComponentCommand(Guid target, std::string component,
                                         std::string before, std::string after)
    : target_(target),
      component_(std::move(component)),
      before_(std::move(before)),
      after_(std::move(after)),
      label_("Set " + component_) {}

std::expected<std::unique_ptr<SetComponentCommand>, Error>
SetComponentCommand::Make(const World& world, Guid target,
                          std::string_view component, std::string after) {
  auto entity = FindEntity(world, target);
  if (!entity) return std::unexpected(std::move(entity.error()));
  auto info = FindComponent(world, component);
  if (!info) return std::unexpected(std::move(info.error()));
  std::string before;
  if (HasComponent(world, **info, *entity)) {
    auto json = (*info)->write_json(world.CReg(), *entity);
    if (!json) return std::unexpected(std::move(json.error()));
    before = std::move(*json);
  }
  return std::make_unique<SetComponentCommand>(
      target, std::string(component), std::move(before), std::move(after));
}

std::expected<void, Error> SetComponentCommand::Apply(World& world) {
  if (after_.empty()) {
    return Fail(ErrorCode::kInvalidArgument, label_ + ": no value");
  }
  if (!applied_) {
    // The value it replaces, read when it first applies: a command made
    // during a panel's Draw applies after the frame's earlier ones, so its
    // value at Make may already be stale (undo must invert this Apply).
    auto entity = FindEntity(world, target_);
    if (!entity) return std::unexpected(std::move(entity.error()));
    auto info = FindComponent(world, component_);
    if (!info) return std::unexpected(std::move(info.error()));
    std::string before;
    if (HasComponent(world, **info, *entity)) {
      auto json = ReadComponent(world, **info, *entity);
      if (!json) return std::unexpected(std::move(json.error()));
      before = std::move(*json);
    }
    if (auto set = SetOrRemove(world, target_, component_, after_); !set) {
      return set;
    }
    before_ = std::move(before);
    applied_ = true;
    return {};
  }
  return SetOrRemove(world, target_, component_, after_);
}

std::expected<void, Error> SetComponentCommand::Revert(World& world) {
  return SetOrRemove(world, target_, component_, before_);
}

std::expected<std::vector<SessionEntry>, Error> SetComponentCommand::ToSession(
    const ComponentRegistry& registry, Tick apply_before) const {
  return One(MakeComponentEntry(registry, SessionOp::kSetComponent,
                                apply_before, target_, component_, after_));
}

bool SetComponentCommand::Absorb(const EditorCommand& next) {
  if (next.Kind() != CommandKind::kSetComponent) return false;
  const auto& set = static_cast<const SetComponentCommand&>(next);
  if (set.target_ != target_ || set.component_ != component_) return false;
  after_ = set.after_;
  return true;
}

// --- AddComponentCommand ---

AddComponentCommand::AddComponentCommand(Guid target, std::string component,
                                         std::string json)
    : target_(target),
      component_(std::move(component)),
      json_(std::move(json)),
      label_("Add " + component_) {}

std::expected<void, Error> AddComponentCommand::Apply(World& world) {
  auto entity = FindEntity(world, target_);
  if (!entity) return std::unexpected(std::move(entity.error()));
  auto info = FindComponent(world, component_);
  if (!info) return std::unexpected(std::move(info.error()));
  if (HasComponent(world, **info, *entity)) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("the entity already has '{}'", component_));
  }
  return WriteComponent(world, **info, *entity, json_);
}

std::expected<void, Error> AddComponentCommand::Revert(World& world) {
  return SetOrRemove(world, target_, component_, {});
}

std::expected<std::vector<SessionEntry>, Error> AddComponentCommand::ToSession(
    const ComponentRegistry& registry, Tick apply_before) const {
  return One(MakeComponentEntry(registry, SessionOp::kAddComponent,
                                apply_before, target_, component_, json_));
}

// --- RemoveComponentCommand ---

RemoveComponentCommand::RemoveComponentCommand(Guid target,
                                               std::string component)
    : target_(target),
      component_(std::move(component)),
      label_("Remove " + component_) {}

std::expected<void, Error> RemoveComponentCommand::Apply(World& world) {
  auto entity = FindEntity(world, target_);
  if (!entity) return std::unexpected(std::move(entity.error()));
  auto info = FindComponent(world, component_);
  if (!info) return std::unexpected(std::move(info.error()));
  auto before = ReadComponent(world, **info, *entity);
  if (!before) return std::unexpected(std::move(before.error()));
  before_ = std::move(*before);
  RemoveComponent(world, **info, *entity);
  return {};
}

std::expected<void, Error> RemoveComponentCommand::Revert(World& world) {
  if (before_.empty()) {
    return Fail(ErrorCode::kInvalidArgument, label_ + " was never applied");
  }
  return SetOrRemove(world, target_, component_, before_);
}

std::expected<std::vector<SessionEntry>, Error>
RemoveComponentCommand::ToSession(const ComponentRegistry& registry,
                                  Tick apply_before) const {
  return One(MakeRemoveComponent(registry, apply_before, target_, component_));
}

// --- CreateEntityCommand ---

CreateEntityCommand::CreateEntityCommand(Guid target,
                                         std::vector<ComponentJson> components)
    : target_(target), components_(std::move(components)) {}

std::expected<void, Error> CreateEntityCommand::Apply(World& world) {
  return CreateWith(world, target_, components_);
}

std::expected<void, Error> CreateEntityCommand::Revert(World& world) {
  return DestroyGuid(world, target_);
}

std::expected<std::vector<SessionEntry>, Error> CreateEntityCommand::ToSession(
    const ComponentRegistry& registry, Tick apply_before) const {
  const std::vector<ComponentValue> values = AsValues(components_);
  return One(MakeCreateEntity(registry, apply_before, target_, values));
}

// --- DestroyEntityCommand ---

DestroyEntityCommand::DestroyEntityCommand(Guid target) : target_(target) {}

std::expected<void, Error> DestroyEntityCommand::Apply(World& world) {
  auto components = SerializeEntity(world, target_);
  if (!components) return std::unexpected(std::move(components.error()));
  components_ = std::move(*components);
  return DestroyGuid(world, target_);
}

std::expected<void, Error> DestroyEntityCommand::Revert(World& world) {
  if (components_.empty()) {
    return Fail(ErrorCode::kInvalidArgument,
                "Destroy entity was never applied");
  }
  return CreateWith(world, target_, components_);
}

std::expected<std::vector<SessionEntry>, Error> DestroyEntityCommand::ToSession(
    const ComponentRegistry& /*registry*/, Tick apply_before) const {
  std::vector<SessionEntry> entries;
  entries.push_back(MakeDestroyEntity(apply_before, target_));
  return entries;
}

// --- PathPointCommand ---

PathPointCommand::PathPointCommand(CommandKind kind, Guid target,
                                   std::string component, std::uint32_t index,
                                   Vec2 point, std::string before,
                                   std::string after)
    : kind_(kind),
      target_(target),
      component_(std::move(component)),
      index_(index),
      point_(point),
      before_(std::move(before)),
      after_(std::move(after)) {}

std::expected<std::unique_ptr<PathPointCommand>, Error> PathPointCommand::Make(
    CommandKind kind, const World& world, Guid target, std::uint32_t index,
    Vec2 point, std::string_view component) {
  auto entity = FindEntity(world, target);
  if (!entity) return std::unexpected(std::move(entity.error()));
  auto info = FindComponent(world, component);
  if (!info) return std::unexpected(std::move(info.error()));
  auto before = ReadComponent(world, **info, *entity);
  if (!before) return std::unexpected(std::move(before.error()));
  auto after = Edit(kind, *before, index, &point);
  if (!after) return std::unexpected(std::move(after.error()));
  return std::unique_ptr<PathPointCommand>(
      new PathPointCommand(kind, target, std::string(component), index, point,
                           std::move(*before), std::move(*after)));
}

std::expected<std::string, Error> PathPointCommand::Edit(
    CommandKind kind, std::string_view before, std::uint32_t index,
    Vec2* point) {
  auto path = json::Parse(before);
  if (!path) return std::unexpected(std::move(path.error()));
  auto points = PointsOfValue(*path);
  if (!points) return std::unexpected(std::move(points.error()));
  std::vector<json::Value>& items = path->Find("points")->items;
  const std::size_t count = items.size();
  switch (kind) {
    case CommandKind::kInsertPathPoint:
      if (index > count) {
        return Fail(
            ErrorCode::kInvalidArgument,
            std::format("insert at {} of a {}-point path", index, count));
      }
      items.insert(items.begin() + index, PointValue(*point));
      break;
    case CommandKind::kMovePathPoint: {
      if (index >= count) {
        return Fail(
            ErrorCode::kInvalidArgument,
            std::format("move of point {} of a {}-point path", index, count));
      }
      json::Value& item = items[index];
      item.Set("x", json::Value::Float(point->x));
      item.Set("y", json::Value::Float(point->y));
      break;
    }
    case CommandKind::kDeletePathPoint:
      if (index >= count) {
        return Fail(
            ErrorCode::kInvalidArgument,
            std::format("delete of point {} of a {}-point path", index, count));
      }
      *point = (*points)[index];
      items.erase(items.begin() + index);
      break;
    default:
      return Fail(ErrorCode::kInvalidArgument, "not a path-point command");
  }
  if (items.size() != count) ClampProgress(*path, items.size());
  return json::Write(*path);
}

std::expected<std::unique_ptr<PathPointCommand>, Error>
PathPointCommand::Insert(const World& world, Guid target, std::uint32_t index,
                         Vec2 point, std::string_view component) {
  return Make(CommandKind::kInsertPathPoint, world, target, index, point,
              component);
}

std::expected<std::unique_ptr<PathPointCommand>, Error> PathPointCommand::Move(
    const World& world, Guid target, std::uint32_t index, Vec2 point,
    std::string_view component) {
  return Make(CommandKind::kMovePathPoint, world, target, index, point,
              component);
}

std::expected<std::unique_ptr<PathPointCommand>, Error>
PathPointCommand::Delete(const World& world, Guid target, std::uint32_t index,
                         std::string_view component) {
  return Make(CommandKind::kDeletePathPoint, world, target, index, Vec2{},
              component);
}

std::string_view PathPointCommand::Label() const noexcept {
  switch (kind_) {
    case CommandKind::kInsertPathPoint:
      return "Insert path point";
    case CommandKind::kMovePathPoint:
      return "Move path point";
    default:
      return "Delete path point";
  }
}

std::expected<void, Error> PathPointCommand::Apply(World& world) {
  if (!applied_) {
    // The path as it is when the command first applies: another command
    // of the same frame may have changed it since Make, and undo must
    // invert this Apply (the edit is made again on the current points).
    auto entity = FindEntity(world, target_);
    if (!entity) return std::unexpected(std::move(entity.error()));
    auto info = FindComponent(world, component_);
    if (!info) return std::unexpected(std::move(info.error()));
    auto before = ReadComponent(world, **info, *entity);
    if (!before) return std::unexpected(std::move(before.error()));
    Vec2 point = point_;
    auto after = Edit(kind_, *before, index_, &point);
    if (!after) return std::unexpected(std::move(after.error()));
    if (auto set = SetOrRemove(world, target_, component_, *after); !set) {
      return set;
    }
    before_ = std::move(*before);
    after_ = std::move(*after);
    point_ = point;
    applied_ = true;
    return {};
  }
  return SetOrRemove(world, target_, component_, after_);
}

std::expected<void, Error> PathPointCommand::Revert(World& world) {
  return SetOrRemove(world, target_, component_, before_);
}

std::expected<std::vector<SessionEntry>, Error> PathPointCommand::ToSession(
    const ComponentRegistry& registry, Tick apply_before) const {
  return One(MakeComponentEntry(registry, SessionOp::kSetComponent,
                                apply_before, target_, component_, after_));
}

bool PathPointCommand::Absorb(const EditorCommand& next) {
  if (kind_ != CommandKind::kMovePathPoint ||
      next.Kind() != CommandKind::kMovePathPoint) {
    return false;
  }
  const auto& move = static_cast<const PathPointCommand&>(next);
  if (move.target_ != target_ || move.component_ != component_ ||
      move.index_ != index_) {
    return false;
  }
  point_ = move.point_;
  after_ = move.after_;
  return true;
}

std::expected<std::vector<Vec2>, Error> PathPointCommand::PointsOf(
    std::string_view json) {
  auto path = json::Parse(json);
  if (!path) return std::unexpected(std::move(path.error()));
  return PointsOfValue(*path);
}

// --- CommandGroup ---

CommandGroup::CommandGroup(std::string label) : label_(std::move(label)) {}

void CommandGroup::Add(std::unique_ptr<EditorCommand> command) {
  if (command == nullptr) return;
  if (!commands_.empty() && commands_.back()->Absorb(*command)) return;
  commands_.push_back(std::move(command));
}

std::unique_ptr<EditorCommand> CommandGroup::TakeSingle() {
  if (commands_.size() != 1) return nullptr;
  std::unique_ptr<EditorCommand> single = std::move(commands_.front());
  commands_.clear();
  return single;
}

Guid CommandGroup::Target() const noexcept {
  if (commands_.empty()) return Guid{};
  const Guid first = commands_.front()->Target();
  for (const auto& command : commands_) {
    if (command->Target() != first) return Guid{};
  }
  return first;
}

std::expected<void, Error> CommandGroup::Apply(World& world) {
  for (std::size_t i = 0; i < commands_.size(); ++i) {
    if (auto applied = commands_[i]->Apply(world); !applied) {
      // Undo the ones that applied, newest first. Each was applied just
      // now, so its revert finds what it changed.
      for (std::size_t j = i; j > 0; --j) {
        if (auto reverted = commands_[j - 1]->Revert(world); !reverted) {
          SE_LOG_ERROR("'{}': undoing '{}' after a failed apply: {}", label_,
                       commands_[j - 1]->Label(), reverted.error().message);
        }
      }
      return applied;
    }
  }
  return {};
}

std::expected<void, Error> CommandGroup::Revert(World& world) {
  for (std::size_t i = commands_.size(); i > 0; --i) {
    if (auto reverted = commands_[i - 1]->Revert(world); !reverted) {
      return reverted;
    }
  }
  return {};
}

std::expected<std::vector<SessionEntry>, Error> CommandGroup::ToSession(
    const ComponentRegistry& registry, Tick apply_before) const {
  std::vector<SessionEntry> entries;
  for (const auto& command : commands_) {
    auto more = command->ToSession(registry, apply_before);
    if (!more) return more;
    for (SessionEntry& entry : *more) entries.push_back(std::move(entry));
  }
  return entries;
}

}  // namespace se::editor
