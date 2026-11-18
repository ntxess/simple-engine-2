// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor_panels/path_commands.h"

#include <entt/entity/registry.hpp>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor_panels/panel_support.h"
#include "se_editor_panels/waypoint_canvas.h"

namespace se::editor {
namespace {

using Commands = std::vector<std::unique_ptr<EditorCommand>>;

bool ProgressAtStart(const rampage::Path& path) noexcept {
  return path.segment == 0 && path.u == 0.0F && !path.done;
}

std::expected<std::unique_ptr<EditorCommand>, Error> PointCommand(
    const World& world, Guid target, const PathPointOp& op) {
  std::expected<std::unique_ptr<PathPointCommand>, Error> command;
  switch (op.kind) {
    case PathPointOp::Kind::kInsert:
      command = PathPointCommand::Insert(world, target, op.index, op.pos);
      break;
    case PathPointOp::Kind::kMove:
      command = PathPointCommand::Move(world, target, op.index, op.pos);
      break;
    case PathPointOp::Kind::kErase:
      command = PathPointCommand::Delete(world, target, op.index);
      break;
  }
  if (!command) return std::unexpected(std::move(command.error()));
  return std::unique_ptr<EditorCommand>(std::move(*command));
}

// Appends |command| unless it is null (nothing changed) or an error.
std::expected<void, Error> Add(
    Commands& out,
    std::expected<std::unique_ptr<EditorCommand>, Error> command) {
  if (!command) return std::unexpected(std::move(command.error()));
  if (*command != nullptr) out.push_back(std::move(*command));
  return {};
}

}  // namespace

std::string_view PathEditLabel(PathEditKind kind) noexcept {
  switch (kind) {
    case PathEditKind::kAppend:
      return "Add path point";
    case PathEditKind::kRemoveLast:
      return "Remove path point";
    case PathEditKind::kClear:
      return "Clear path";
  }
  return "Edit path";
}

std::expected<Commands, Error> PathEditCommands(const World& world, Guid target,
                                                const PathEdit& edit) {
  const std::optional<entt::entity> entity = world.Find(target);
  if (!entity) return Fail(ErrorCode::kNotFound, "no such entity");
  const entt::registry& reg = world.CReg();
  const auto* path = reg.try_get<rampage::Path>(*entity);
  const auto* transform = reg.try_get<Transform>(*entity);
  if (path == nullptr || transform == nullptr) {
    return Fail(ErrorCode::kNotFound, "the entity has no Path and Transform");
  }

  Commands commands;
  const bool loop_stays = !edit.loop.has_value() || *edit.loop == path->loop;
  if (edit.ops.size() == 1 && !edit.entity_pos.has_value() && loop_stays &&
      (!edit.restart || ProgressAtStart(*path))) {
    if (auto added = Add(commands, PointCommand(world, target, edit.ops[0]));
        !added) {
      return std::unexpected(std::move(added.error()));
    }
    return commands;
  }

  rampage::Path after = *path;
  Vec2 pos = transform->pos;
  ApplyPathEdit(edit, after, pos);
  auto before = ReadComponentJson(world, *entity, "Path");
  if (!before) return std::unexpected(std::move(before.error()));
  if (auto added = Add(commands, SetFromValue(world, target, "Path",
                                              std::move(*before), after));
      !added) {
    return std::unexpected(std::move(added.error()));
  }
  if (pos != transform->pos) {
    Transform moved = *transform;
    moved.pos = pos;
    auto json = ReadComponentJson(world, *entity, "Transform");
    if (!json) return std::unexpected(std::move(json.error()));
    if (auto added = Add(commands, SetFromValue(world, target, "Transform",
                                                std::move(*json), moved));
        !added) {
      return std::unexpected(std::move(added.error()));
    }
  }
  if (reg.all_of<rampage::PathCache>(*entity)) {
    auto json = ReadComponentJson(world, *entity, "PathCache");
    if (!json) return std::unexpected(std::move(json.error()));
    if (auto added =
            Add(commands, SetFromValue(world, target, "PathCache",
                                       std::move(*json), rampage::PathCache{}));
        !added) {
      return std::unexpected(std::move(added.error()));
    }
  }
  return commands;
}

}  // namespace se::editor
