// Copyright 2026 The simple-engine-2 Authors.
//
// The waypoint canvas's rules as EditorCommands (ADR-100). A PathEdit
// (se_editor_panels/waypoint_canvas.h) becomes:
//   - one path-point command (se_editor/commands.h: insert, move or delete
//     one point) when the rule changes one point and nothing else: the
//     entity already stands on point 0, the path's progress is at its
//     start (always so in an Edit document, which is never stepped) and the
//     loop flag stays;
//   - otherwise a set of the whole Path (the edited copy: points, restart,
//     loop), a set of the Transform when the entity moves to point 0, and,
//     when the entity has a PathCache, a reset of the cache so the
//     PathSystem rebuilds it (a moved point keeps the point count, which is
//     all the cache's staleness test looks at).
// The Inspector submits them in one coalesce scope named after the rule, so
// each click or menu item is one undo step.

#ifndef SE_EDITOR_PANELS_PATH_COMMANDS_H_
#define SE_EDITOR_PANELS_PATH_COMMANDS_H_

#include <expected>
#include <memory>
#include <string_view>
#include <vector>

#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor_panels/waypoint_canvas.h"

namespace se::editor {

// The undo label of a rule ("Add path point", "Remove path point", "Clear
// path").
[[nodiscard]] std::string_view PathEditLabel(PathEditKind kind) noexcept;

// The commands for |edit| of the entity |target| in |world| (which must
// have a Path and a Transform).
[[nodiscard]] std::expected<std::vector<std::unique_ptr<EditorCommand>>, Error>
PathEditCommands(const World& world, Guid target, const PathEdit& edit);

}  // namespace se::editor

#endif  // SE_EDITOR_PANELS_PATH_COMMANDS_H_
