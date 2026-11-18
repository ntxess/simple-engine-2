// Copyright 2026 The simple-engine-2 Authors.
//
// EditQueue: the Edit-mode commands of a frame, bound to their documents
// (docs/milestone-specs.md M11b "Commands and the carry-overs"; ADR-118).
// Main thread only.
//
// Panels submit commands, coalesce begins and ends while they draw; each
// item records the document it was submitted for (the active one then)
// and the panel that was drawing. The frame's apply step (after
// ImGui::Render) applies every item to its own document, in order; an item
// whose document was closed is dropped and logged, and a change of the
// active document never redirects an edit.
//
// A coalesce scope belongs to its document and to the panel that opened
// it. Apply ends it when that panel did not draw in the frame (its tab
// hidden, its window closed); EndScopeOf ends it when its document is
// closed or deactivated, EndScope at Play. Ending a scope is logged. A
// scope opened with no panel (the editor script's coalesce-begin) is ended
// by the document and Play rules only.

#ifndef SE_EDITOR_EDIT_QUEUE_H_
#define SE_EDITOR_EDIT_QUEUE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_editor/commands.h"
#include "se_editor/documents.h"

namespace se::editor {

class EditQueue {
 public:
  // The open scope: its document, the panel that opened it, its label.
  struct Scope {
    DocumentId document = kNoDocument;
    std::string panel;
    std::string label;
  };

  struct Applied {
    std::size_t applied = 0;
    // Items whose document was closed, and commands that failed.
    std::size_t dropped = 0;
    std::size_t failed = 0;
    // The last failure's message.
    std::string error;
  };

  void Submit(DocumentId document, std::string_view panel,
              std::unique_ptr<EditorCommand> command);
  void BeginCoalesce(DocumentId document, std::string_view panel,
                     std::string_view label);
  void EndCoalesce(DocumentId document, std::string_view panel);

  // The frame's apply step: every item to its document, then the scope
  // ends when its panel did not draw this frame (|drew| answers by the
  // panel's name).
  Applied Apply(DocumentSet& documents,
                const std::function<bool(std::string_view panel)>& drew)
      SE_REQUIRES(g_sim_owner);
  // Drops every queued item (Play started: the document is frozen).
  std::size_t DropAll(std::string_view why);

  // Ends the open scope, if any, logging |why|.
  void EndScope(DocumentSet& documents, std::string_view why);
  // Ends the open scope when it belongs to |document|.
  void EndScopeOf(DocumentSet& documents, DocumentId document,
                  std::string_view why);
  [[nodiscard]] const std::optional<Scope>& OpenScope() const noexcept {
    return scope_;
  }
  [[nodiscard]] std::size_t Pending() const noexcept { return items_.size(); }
  // Scopes ended by a rule (not by their own EndCoalesce), so far.
  [[nodiscard]] std::uint64_t ScopesEnded() const noexcept {
    return scopes_ended_;
  }

 private:
  struct Item {
    std::unique_ptr<EditorCommand> command;
    DocumentId document = kNoDocument;
    std::string panel;
    bool begin = false;
    bool end = false;
    std::string label;
  };

  std::vector<Item> items_;
  std::optional<Scope> scope_;
  std::uint64_t scopes_ended_ = 0;
};

}  // namespace se::editor

#endif  // SE_EDITOR_EDIT_QUEUE_H_
