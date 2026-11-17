// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor/edit_queue.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_editor/commands.h"
#include "se_editor/documents.h"
#include "se_editor/edit_document.h"

namespace se::editor {

void EditQueue::Submit(DocumentId document, std::string_view panel,
                       std::unique_ptr<EditorCommand> command) {
  if (command == nullptr) return;
  Item item;
  item.command = std::move(command);
  item.document = document;
  item.panel = std::string(panel);
  items_.push_back(std::move(item));
}

void EditQueue::BeginCoalesce(DocumentId document, std::string_view panel,
                              std::string_view label) {
  Item item;
  item.document = document;
  item.panel = std::string(panel);
  item.begin = true;
  item.label = std::string(label);
  items_.push_back(std::move(item));
}

void EditQueue::EndCoalesce(DocumentId document, std::string_view panel) {
  Item item;
  item.document = document;
  item.panel = std::string(panel);
  item.end = true;
  items_.push_back(std::move(item));
}

EditQueue::Applied EditQueue::Apply(
    DocumentSet& documents,
    const std::function<bool(std::string_view panel)>& drew) {
  Applied result;
  std::vector<Item> items = std::move(items_);
  items_.clear();
  for (Item& item : items) {
    DocumentEntry* entry = documents.Find(item.document);
    if (entry == nullptr) {
      if (item.command != nullptr) {
        ++result.dropped;
        SE_LOG_WARN(
            "editor: '{}' dropped: the document it was made for ({}) is "
            "closed",
            item.command->Label(), item.document);
      }
      continue;
    }
    EditDocument& document = *entry->document;
    if (item.begin) {
      // One scope at a time: a scope of another document ends first.
      if (scope_ && scope_->document != item.document) {
        EndScope(documents, "another scope began");
      }
      document.BeginCoalesce(item.label);
      scope_ = Scope{item.document, item.panel, item.label};
      continue;
    }
    if (item.end) {
      document.EndCoalesce();
      if (scope_ && scope_->document == item.document) scope_.reset();
      continue;
    }
    const std::string label(item.command->Label());
    if (auto applied = document.Apply(std::move(item.command)); !applied) {
      ++result.failed;
      result.error = applied.error().message;
      SE_LOG_ERROR("edit '{}': {}", label, applied.error().message);
      continue;
    }
    ++result.applied;
  }
  if (scope_) {
    DocumentEntry* entry = documents.Find(scope_->document);
    if (entry == nullptr || !entry->document->Coalescing()) {
      // Ended elsewhere (an undo, a save).
      scope_.reset();
    } else if (!scope_->panel.empty() && drew && !drew(scope_->panel)) {
      EndScope(documents,
               "its panel '" + scope_->panel + "' did not draw in the frame");
    }
  }
  return result;
}

std::size_t EditQueue::DropAll(std::string_view why) {
  std::size_t dropped = 0;
  for (const Item& item : items_) {
    if (item.command == nullptr) continue;
    ++dropped;
    SE_LOG_WARN("editor: '{}' dropped: {}", item.command->Label(), why);
  }
  items_.clear();
  return dropped;
}

void EditQueue::EndScope(DocumentSet& documents, std::string_view why) {
  if (!scope_) return;
  const Scope scope = std::move(*scope_);
  scope_.reset();
  DocumentEntry* entry = documents.Find(scope.document);
  if (entry == nullptr || !entry->document->Coalescing()) return;
  entry->document->EndCoalesce();
  ++scopes_ended_;
  SE_LOG_INFO("editor: coalesce scope '{}'{} of document {} ended: {}",
              scope.label,
              scope.panel.empty() ? std::string() : " of '" + scope.panel + "'",
              scope.document, why);
}

void EditQueue::EndScopeOf(DocumentSet& documents, DocumentId document,
                           std::string_view why) {
  if (scope_ && scope_->document == document) EndScope(documents, why);
}

}  // namespace se::editor
