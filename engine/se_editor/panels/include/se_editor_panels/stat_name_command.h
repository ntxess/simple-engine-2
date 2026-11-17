// Copyright 2026 The simple-engine-2 Authors.
//
// StatNameCommand: a stat's name recorded in the document World's
// StatNames (docs/milestone-specs.md M11b "Commands and the carry-overs";
// ADR-119). A stat typed by name in the Stats or effect drawers used to
// live in the drawer (ADR-100), so the scene writer saved it in registry
// form and the name was lost; now the drawers submit a new stat as one
// undo step {StatNameCommand, SetComponentCommand}, so the name reaches
// the document through a command and the file through the writer.
//
//   Apply   records the name under the kEditorCommand gate (World::Reg),
//           remembering whether it was new; a name whose id another name
//           already has is an error (the World could not tell the two
//           stats apart);
//   Revert  removes a name it added (NameTable::Remove);
//   ToSession  no entries: stat ids are name hashes, so the play World
//           needs no name (the Play-start bytes carry the names in the
//           scene's own form).

#ifndef SE_EDITOR_PANELS_STAT_NAME_COMMAND_H_
#define SE_EDITOR_PANELS_STAT_NAME_COMMAND_H_

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "game_rampage/components.h"
#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_sim/session_log.h"

namespace se::editor {

class StatNameCommand final : public EditorCommand {
 public:
  explicit StatNameCommand(std::string name);

  [[nodiscard]] CommandKind Kind() const noexcept override {
    return CommandKind::kGame;
  }
  [[nodiscard]] std::string_view Label() const noexcept override {
    return label_;
  }
  [[nodiscard]] Guid Target() const noexcept override { return Guid{}; }
  [[nodiscard]] std::expected<void, Error> Apply(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<void, Error> Revert(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<std::vector<SessionEntry>, Error> ToSession(
      const ComponentRegistry& registry, Tick apply_before) const override;

  [[nodiscard]] const std::string& StatName() const noexcept { return name_; }
  [[nodiscard]] rampage::StatId Id() const noexcept { return id_; }
  // The last Apply added the name (it was not in the World's StatNames).
  [[nodiscard]] bool Added() const noexcept { return added_; }

 private:
  std::string name_;
  rampage::StatId id_;
  std::string label_;
  bool added_ = false;
};

// Whether |world|'s StatNames hold |name|.
[[nodiscard]] bool HasStatName(const World& world, std::string_view name);

// |set| with the names it introduces: a CommandGroup {StatNameCommand per
// name, |set|} labelled |label|, one undo step. |set| null: the names
// alone.
[[nodiscard]] std::unique_ptr<EditorCommand> WithStatNames(
    std::string label, const std::vector<std::string>& names,
    std::unique_ptr<EditorCommand> set);

// The Stats drawer's new stat: stat |name| = |value| on |guid|'s Stats (the
// component added when it has none), with its name.
[[nodiscard]] std::expected<std::unique_ptr<EditorCommand>, Error>
MakeStatNameEdit(const World& world, Guid guid, std::string_view name,
                 float value);

}  // namespace se::editor

#endif  // SE_EDITOR_PANELS_STAT_NAME_COMMAND_H_
