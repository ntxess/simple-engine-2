// Copyright 2026 The simple-engine-2 Authors.
//
// EditDocument: one open scene (docs/milestone-specs.md M10a "EditDocument";
// docs/design/mt-design.md "Editor", documents; ADR-096). A World of kind
// kEditDocument that is never stepped, opened from a scene file with the
// game's loader (M3), with its own undo stack, selection and dirty flag.
// Main thread only.
//
// Every change is an EditorCommand applied by Apply() under the World's
// kEditorCommand gate; the gate is closed otherwise, so any other mutation
// of the World fails at once (World::Reg asserts). Apply records the
// command on the undo stack (into the open coalesce group, if any); Undo
// and Redo walk it. Revision() counts the changes; the document is dirty
// while its revision differs from the one last saved.

#ifndef SE_EDITOR_EDIT_DOCUMENT_H_
#define SE_EDITOR_EDIT_DOCUMENT_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/editor_game.h"
#include "se_editor/selection.h"

namespace se::editor {

struct DocumentDesc {
  // The scene file (where Save writes); empty for an unsaved document.
  std::string path;
  // The document's name (the scene's "name"), for its title and saves.
  std::string name;
  // Prefab references resolve here.
  std::string asset_root;
  std::uint64_t seed = 0;
  std::uint32_t tick_rate = kDefaultTickRate;
};

class EditDocument {
 public:
  // Loads |text| (a scene document) with |game|'s loader into a new
  // document World.
  [[nodiscard]] static std::expected<std::unique_ptr<EditDocument>, Error> Open(
      const ComponentRegistry& registry, const EditorGame& game,
      DocumentDesc desc, std::string_view text) SE_REQUIRES(g_sim_owner);

  EditDocument(const EditDocument&) = delete;
  EditDocument& operator=(const EditDocument&) = delete;
  ~EditDocument();

  [[nodiscard]] const World& Get() const noexcept { return *world_; }
  [[nodiscard]] const DocumentDesc& Desc() const noexcept { return desc_; }
  // The World as a GameState holds it (the editor's document state, which
  // the AssetRuntime's scope and the App's render sources read). Changing
  // it still takes the command gate: World::Reg asserts it.
  [[nodiscard]] World* StateWorld() noexcept { return world_.get(); }
  [[nodiscard]] Selection& GetSelection() noexcept { return selection_; }
  [[nodiscard]] const Selection& GetSelection() const noexcept {
    return selection_;
  }

  // Applies |command| under the command gate and records it. A command
  // that fails changes nothing and is dropped.
  [[nodiscard]] std::expected<void, Error> Apply(
      std::unique_ptr<EditorCommand> command) SE_REQUIRES(g_sim_owner);
  // A drag: the commands applied until EndCoalesce form one undo step.
  // A second Begin ends the open group first.
  void BeginCoalesce(std::string_view label);
  void EndCoalesce();
  [[nodiscard]] bool Coalescing() const noexcept { return group_ != nullptr; }

  // False when there is nothing to undo or redo. An undo ends an open
  // coalesce group first.
  [[nodiscard]] std::expected<bool, Error> Undo() SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<bool, Error> Redo() SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::size_t UndoCount() const noexcept;
  [[nodiscard]] std::size_t RedoCount() const noexcept {
    return undone_.size();
  }
  // The label of the step Undo or Redo would take ("" when none).
  [[nodiscard]] std::string_view UndoLabel() const noexcept;
  [[nodiscard]] std::string_view RedoLabel() const noexcept;

  // Bumped by every applied, undone or redone change.
  [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }
  [[nodiscard]] bool Dirty() const noexcept {
    return revision_ != saved_revision_;
  }
  // |revision| reached the file (a save completed). A later change keeps
  // the document dirty.
  void MarkSaved(std::uint64_t revision) noexcept;
  // Saves go to |path| from now on (Save As).
  void SetPath(std::string path) { desc_.path = std::move(path); }

  // The document as a scene file, with the game's writer.
  [[nodiscard]] std::expected<std::string, Error> Serialize() const;
  // The document World's hash.
  [[nodiscard]] std::uint64_t Hash() const;

 private:
  EditDocument(const EditorGame& game, DocumentDesc desc,
               std::unique_ptr<World> world) noexcept;
  void Record(std::unique_ptr<EditorCommand> command);
  [[nodiscard]] World& Mutable() SE_REQUIRES(g_sim_owner) { return *world_; }

  const EditorGame* game_;
  DocumentDesc desc_;
  std::unique_ptr<World> world_;
  Selection selection_;
  std::vector<std::unique_ptr<EditorCommand>> done_;
  std::vector<std::unique_ptr<EditorCommand>> undone_;
  std::unique_ptr<CommandGroup> group_;
  std::uint64_t revision_ = 0;
  std::uint64_t saved_revision_ = 0;
};

}  // namespace se::editor

#endif  // SE_EDITOR_EDIT_DOCUMENT_H_
