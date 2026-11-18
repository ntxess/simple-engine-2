// Copyright 2026 The simple-engine-2 Authors.
//
// Prefab-aware commands and the document file rules (docs/milestone-specs.md
// M11b "Commands and the carry-overs", "Hot reload into documents";
// ADR-119, ADR-120). Built from the game's scene hooks (EditorGame) and the
// ComponentRegistry's JSON hooks only, so se_editor stays game-agnostic:
//
//   - removal: a component goes with the components derived from it
//     (EditorGame::removed_with) in one CommandGroup, one undo step; a
//     component the game does not allow to be removed is refused;
//   - "Revert to prefab": a set-component command to the prefab baseline's
//     value (an add when the instance had removed it, a remove when the
//     prefab lacks it);
//   - override markers: the editor-visible components that differ from the
//     baseline;
//   - a prefab change: the set, add and remove commands that turn every
//     instance into what EditorGame::rebase_instances gives, as one
//     CommandGroup "Prefab changed: <ref>";
//   - a document's own file changed on disk: unchanged content is ignored,
//     a clean document reloads, a dirty one asks.
// The entity's identity and bookkeeping (IdComponent, Name, Spawned,
// PrefabInstance, DestroyRequest) are never touched by a prefab change, a
// revert or a marker.

#ifndef SE_EDITOR_PREFAB_COMMANDS_H_
#define SE_EDITOR_PREFAB_COMMANDS_H_

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/edit_document.h"
#include "se_editor/editor_game.h"

namespace se::editor {

// The core component that records an entity's prefab (M11a, se_core).
inline constexpr std::string_view kPrefabInstanceComponent = "PrefabInstance";

// IdComponent, Name, Spawned, PrefabInstance and DestroyRequest.
[[nodiscard]] bool PrefabReserved(std::string_view component) noexcept;

// The prefab |guid| is an instance of (its PrefabInstance's "prefab"), or
// nullopt (none, or no such entity).
[[nodiscard]] std::optional<std::string> PrefabOf(const World& world,
                                                  Guid guid);

// Whether the entity |guid| holds |component|.
[[nodiscard]] bool HoldsComponent(const World& world, Guid guid,
                                  std::string_view component);

// Whether |game| allows |component| to be removed (true without the hook).
[[nodiscard]] bool RemovableBy(const EditorGame& game,
                               std::string_view component);

// A CommandGroup "Remove <component>" of RemoveComponentCommands: the
// component, then each component |game| says goes with it that the entity
// holds. An error naming the reason when the game refuses the removal or
// the entity lacks the component.
[[nodiscard]] std::expected<std::unique_ptr<EditorCommand>, Error> MakeRemoval(
    const World& world, Guid guid, std::string_view component,
    const EditorGame& game);

// The baseline's JSON of |component| (nullopt: the prefab lacks it).
[[nodiscard]] std::optional<std::string> BaselineJson(
    const EntityJson& baseline, std::string_view component);

// "Revert to prefab" of |component|: a set to the baseline's value (an add
// when the entity lacks it), or a remove when the baseline lacks it. Null
// when the component already equals the baseline.
[[nodiscard]] std::expected<std::unique_ptr<EditorCommand>, Error>
MakeRevertToPrefab(const World& world, Guid guid, std::string_view component,
                   const EntityJson& baseline);

// Whether |component| of |guid| differs from |baseline|: held by one of
// them only, or with different JSON.
[[nodiscard]] bool DiffersFromBaseline(const World& world, Guid guid,
                                       std::string_view component,
                                       const EntityJson& baseline);

// The editor-visible, serialized components (reserved ones aside) that
// differ from |baseline|, in registration order.
[[nodiscard]] std::vector<std::string> OverriddenComponents(
    const World& world, Guid guid, const EntityJson& baseline);

// The commands that turn each instance of |results| into its entry, as one
// CommandGroup labelled |label|; null when nothing would change.
[[nodiscard]] std::expected<std::unique_ptr<EditorCommand>, Error> MakeRebase(
    const World& world, std::string label,
    const std::vector<EntityJson>& results);

// A scene document's rebase onto |new_text| of prefab |ref|, against the
// text its load recorded: one undo step "Prefab changed: <ref>" when an
// instance changes (EditDocument::ApplyPrefabChange: clean stays clean
// when its file's text would not change). The recorded text becomes
// |new_text| whenever the rebase succeeds. False when the document holds
// no record of |ref|, the text is the recorded one, or no instance
// changed.
[[nodiscard]] std::expected<bool, Error> RebaseDocument(
    EditDocument& document, const EditorGame& game, std::string_view ref,
    const std::string& new_text) SE_REQUIRES(g_sim_owner);

// What a change of a document's own file on disk means.
enum class FileTextAction : std::uint8_t {
  // The text is what the document's file held: nothing to do.
  kUnchanged,
  // A clean document: reload it (its undo stack starts again).
  kReload,
  // A dirty document: ask (Reload, Keep mine).
  kAsk,
};

[[nodiscard]] FileTextAction ClassifyFileText(const EditDocument& document,
                                              std::string_view text) noexcept;

// |document| opened again from |text| (its desc, its game): the reload.
[[nodiscard]] std::expected<std::unique_ptr<EditDocument>, Error>
ReopenDocument(const EditDocument& document, const EditorGame& game,
               std::string_view text) SE_REQUIRES(g_sim_owner);

}  // namespace se::editor

#endif  // SE_EDITOR_PREFAB_COMMANDS_H_
