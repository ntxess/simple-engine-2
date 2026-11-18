// Copyright 2026 The simple-engine-2 Authors.
//
// EditDocument: one open scene or prefab (docs/milestone-specs.md M10a
// "EditDocument", M11b "Documents"; docs/design/mt-design.md "Editor",
// documents; ADR-096, ADR-117). A World of kind kEditDocument that is never
// stepped, opened from a scene file with the game's loader (M3, M11a) or
// from a prefab file as a World with the prefab's one root entity, with its
// own undo stack, selection and dirty flag. Main thread only.
//
// Every change is an EditorCommand applied by Apply() under the World's
// kEditorCommand gate; the gate is closed otherwise, so any other mutation
// of the World fails at once (World::Reg asserts). Apply records the
// command on the undo stack (into the open coalesce group, if any); Undo
// and Redo walk it. Revision() counts the changes; the document is dirty
// while its revision differs from the one last saved.
//
// Two hashes describe a document: Hash(), the World's (kHashed components
// only), and ContentHash(), the hash of its flattened scene text (the
// game's save_scene), which also covers what the World hash cannot see:
// SpriteRenderer, Camera2D, IdComponent, PrefabInstance and the name
// tables. Undo, redo and Stop's check use both.
//
// A scene document keeps the text of every prefab its load read (by
// reference); a change of a prefab's file is rebased against it. The
// file's text as last loaded or saved (FileText) decides whether a change
// on disk is new content and whether a prefab change leaves the document
// clean (decision 2 of 2026-11-17).

#ifndef SE_EDITOR_EDIT_DOCUMENT_H_
#define SE_EDITOR_EDIT_DOCUMENT_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
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

enum class DocumentKind : std::uint8_t {
  kScene,
  kPrefab,
};

[[nodiscard]] std::string_view DocumentKindName(DocumentKind kind) noexcept;

struct DocumentDesc {
  DocumentKind kind = DocumentKind::kScene;
  // The file (where Save writes); empty for an unsaved document.
  std::string path;
  // The document's name (the scene's or the prefab's "name"), for its
  // title and saves.
  std::string name;
  // A prefab document: its reference relative to the asset root
  // ("prefabs/enemy.prefab.json").
  std::string ref;
  // Prefab references resolve here.
  std::string asset_root;
  std::uint64_t seed = 0;
  std::uint32_t tick_rate = kDefaultTickRate;
};

class EditDocument {
 public:
  // Loads |text| (a scene document, or a prefab when desc.kind is
  // kPrefab) with |game|'s loader into a new document World. The prefab
  // texts the load reported are kept; |text| is the file's text.
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
  // The open scope's label ("" when none).
  [[nodiscard]] std::string_view CoalesceLabel() const noexcept;

  // A prefab change (decision 2): |command| applies as one undo step; the
  // document stays clean when it was clean and its file's text would not
  // change (the instances now load as the file says).
  [[nodiscard]] std::expected<void, Error> ApplyPrefabChange(
      std::unique_ptr<EditorCommand> command) SE_REQUIRES(g_sim_owner);

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

  // The document as a flattened scene (the game's save_scene): the
  // Play-start bytes and the content hash.
  [[nodiscard]] std::expected<std::string, Error> Serialize() const;
  // The document as its file: a scene with prefab references
  // (save_scene_file, kPrefabs) or a prefab (save_prefab). Save, Save As,
  // autosave and recovery.
  [[nodiscard]] std::expected<std::string, Error> SerializeFile() const;
  // Autosaves and recovery files: SerializeFile(), or when a prefab file
  // it reads cannot be read, the scene with its overrides against the
  // prefab texts the document recorded, else flattened (Serialize()): the
  // unsaved edits are never lost to a broken prefab file.
  [[nodiscard]] std::expected<std::string, Error> SerializeRecovery() const;
  // The document World's hash.
  [[nodiscard]] std::uint64_t Hash() const;
  // The hash of Serialize()'s text (0 when it fails).
  [[nodiscard]] std::uint64_t ContentHash() const;

  // The prefab texts the load reported, by reference (scene documents).
  [[nodiscard]] const PrefabTextMap& PrefabTexts() const noexcept {
    return prefab_texts_;
  }
  // The text a prefab change was rebased onto (not part of undo).
  void SetPrefabText(std::string_view ref, std::string text);
  // The file's text as last loaded or saved.
  [[nodiscard]] const std::string& FileText() const noexcept {
    return file_text_;
  }
  void SetFileText(std::string text) { file_text_ = std::move(text); }

 private:
  EditDocument(const EditorGame& game, DocumentDesc desc,
               std::unique_ptr<World> world);
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
  PrefabTextMap prefab_texts_;
  std::string file_text_;
};

}  // namespace se::editor

#endif  // SE_EDITOR_EDIT_DOCUMENT_H_
