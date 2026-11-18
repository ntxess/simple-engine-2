// Copyright 2026 The simple-engine-2 Authors.
//
// DocumentSet: every document the editor has open (docs/milestone-specs.md
// M11b "Documents"; decision 1 of 2026-11-17; ADR-117). Each document is a
// scene or a prefab with an id that is never reused, its own undo stack,
// selection and dirty flag (EditDocument), its own autosave timer and
// recovery state, and the hot-reload state of its file. One of them is
// active: the panels show it, edits are made for it, Play plays it (a scene
// only). While a Play session exists the set is frozen: no document is
// opened, closed or activated. Main thread only.

#ifndef SE_EDITOR_DOCUMENTS_H_
#define SE_EDITOR_DOCUMENTS_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "se_editor/edit_document.h"
#include "se_editor/recovery.h"

namespace se::editor {

// A document's id: never 0, never reused while the editor runs.
using DocumentId = std::uint64_t;
inline constexpr DocumentId kNoDocument = 0;

// One open document and the editor's state about it.
struct DocumentEntry {
  explicit DocumentEntry(std::uint64_t autosave_quiet_ns) noexcept
      : autosave(autosave_quiet_ns) {}

  DocumentId id = kNoDocument;
  std::unique_ptr<EditDocument> document;
  // Hashes when it was opened (the script's "initial").
  std::uint64_t initial_hash = 0;
  std::uint64_t initial_content_hash = 0;
  // Autosave: the timer, the last change it saw and when.
  AutosaveTimer autosave;
  std::uint64_t seen_revision = 0;
  std::uint64_t last_change_ns = 0;
  // The revision of the last completed save since it was opened: a
  // recovery write of an older revision that lands later is stale.
  std::optional<std::uint64_t> last_save_revision;
  // The last failed save's error ("" after a save submitted).
  std::string save_error;
  // Its own file changed on disk while it was dirty: the prompt (Reload,
  // Keep mine) and the file's new text.
  bool reload_prompt = false;
  std::string reload_text;
};

class DocumentSet {
 public:
  explicit DocumentSet(std::uint64_t autosave_quiet_ns) noexcept
      : autosave_quiet_ns_(autosave_quiet_ns) {}
  DocumentSet(const DocumentSet&) = delete;
  DocumentSet& operator=(const DocumentSet&) = delete;

  // Adds |document| at the end with a new id and activates it. Refused
  // (kNoDocument) while frozen.
  DocumentId Add(std::unique_ptr<EditDocument> document);
  // Replaces document |id| (a reload or a recovery) with |document| under a
  // new id at the same place; the active one stays active. Commands made
  // for the old id find no document. Returns the new id (kNoDocument when
  // |id| is not open or the set is frozen).
  DocumentId Replace(DocumentId id, std::unique_ptr<EditDocument> document);
  // Closes |id|: the next one (else the previous one) becomes active when
  // it was. Returns the document (null when not open or frozen).
  std::unique_ptr<EditDocument> Close(DocumentId id);
  // False when |id| is not open or the set is frozen.
  bool Activate(DocumentId id);

  [[nodiscard]] DocumentEntry* Find(DocumentId id) noexcept;
  [[nodiscard]] const DocumentEntry* Find(DocumentId id) const noexcept;
  // The document open on |path|, or null: the same file however it is
  // spelt (fs::Canonical; on Windows a path differing only in case).
  [[nodiscard]] DocumentEntry* FindByPath(std::string_view path) noexcept;
  // The document whose name is |name|, else the one at index |name|
  // (0-based), as the script's activate names them.
  [[nodiscard]] DocumentEntry* FindByName(std::string_view name) noexcept;
  [[nodiscard]] DocumentEntry* At(std::size_t index) noexcept;

  [[nodiscard]] DocumentEntry* Active() noexcept;
  [[nodiscard]] const DocumentEntry* Active() const noexcept;
  [[nodiscard]] DocumentId ActiveId() const noexcept { return active_; }
  [[nodiscard]] EditDocument* ActiveDocument() noexcept;
  [[nodiscard]] const EditDocument* ActiveDocument() const noexcept;
  // The active document's index, or size() when none.
  [[nodiscard]] std::size_t ActiveIndex() const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::span<const std::unique_ptr<DocumentEntry>> All()
      const noexcept {
    return entries_;
  }
  // The dirty documents, in tab order.
  [[nodiscard]] std::vector<DocumentEntry*> Dirty() const;

  // Play: no open, close or switch while frozen.
  [[nodiscard]] bool Frozen() const noexcept { return frozen_; }
  void SetFrozen(bool frozen) noexcept { frozen_ = frozen; }

  // Every document, for shutdown.
  void Clear() noexcept;

 private:
  [[nodiscard]] std::unique_ptr<DocumentEntry> MakeEntry(
      std::unique_ptr<EditDocument> document);

  std::uint64_t autosave_quiet_ns_;
  std::vector<std::unique_ptr<DocumentEntry>> entries_;
  DocumentId active_ = kNoDocument;
  DocumentId next_id_ = 1;
  bool frozen_ = false;
};

}  // namespace se::editor

#endif  // SE_EDITOR_DOCUMENTS_H_
