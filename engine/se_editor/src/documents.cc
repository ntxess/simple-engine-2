// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor/documents.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "se_editor/edit_document.h"
#include "se_platform/fs.h"

namespace se::editor {

std::unique_ptr<DocumentEntry> DocumentSet::MakeEntry(
    std::unique_ptr<EditDocument> document) {
  auto entry = std::make_unique<DocumentEntry>(autosave_quiet_ns_);
  entry->id = next_id_++;
  entry->initial_hash = document->Hash();
  entry->initial_content_hash = document->ContentHash();
  entry->autosave.Reset(document->Revision());
  entry->seen_revision = document->Revision();
  entry->document = std::move(document);
  return entry;
}

DocumentId DocumentSet::Add(std::unique_ptr<EditDocument> document) {
  if (frozen_ || document == nullptr) return kNoDocument;
  std::unique_ptr<DocumentEntry> entry = MakeEntry(std::move(document));
  active_ = entry->id;
  entries_.push_back(std::move(entry));
  return active_;
}

DocumentId DocumentSet::Replace(DocumentId id,
                                std::unique_ptr<EditDocument> document) {
  if (frozen_ || document == nullptr) return kNoDocument;
  for (std::unique_ptr<DocumentEntry>& entry : entries_) {
    if (entry->id != id) continue;
    entry = MakeEntry(std::move(document));
    if (active_ == id) active_ = entry->id;
    return entry->id;
  }
  return kNoDocument;
}

std::unique_ptr<EditDocument> DocumentSet::Close(DocumentId id) {
  if (frozen_) return nullptr;
  const auto it =
      std::find_if(entries_.begin(), entries_.end(),
                   [id](const std::unique_ptr<DocumentEntry>& entry) {
                     return entry->id == id;
                   });
  if (it == entries_.end()) return nullptr;
  std::unique_ptr<EditDocument> document = std::move((*it)->document);
  const auto index = static_cast<std::size_t>(it - entries_.begin());
  entries_.erase(it);
  if (active_ == id) {
    active_ = entries_.empty()
                  ? kNoDocument
                  : entries_[std::min(index, entries_.size() - 1)]->id;
  }
  return document;
}

bool DocumentSet::Activate(DocumentId id) {
  if (frozen_ || Find(id) == nullptr) return false;
  active_ = id;
  return true;
}

DocumentEntry* DocumentSet::Find(DocumentId id) noexcept {
  for (const std::unique_ptr<DocumentEntry>& entry : entries_) {
    if (entry->id == id) return entry.get();
  }
  return nullptr;
}

const DocumentEntry* DocumentSet::Find(DocumentId id) const noexcept {
  for (const std::unique_ptr<DocumentEntry>& entry : entries_) {
    if (entry->id == id) return entry.get();
  }
  return nullptr;
}

DocumentEntry* DocumentSet::FindByPath(std::string_view path) noexcept {
  if (path.empty()) return nullptr;
  const std::string normal = fs::Normalize(path);
  for (const std::unique_ptr<DocumentEntry>& entry : entries_) {
    if (entry->document->Desc().path == normal) return entry.get();
  }
  return nullptr;
}

DocumentEntry* DocumentSet::FindByName(std::string_view name) noexcept {
  for (const std::unique_ptr<DocumentEntry>& entry : entries_) {
    if (entry->document->Desc().name == name) return entry.get();
  }
  std::size_t index = 0;
  const auto [end, error] =
      std::from_chars(name.data(), name.data() + name.size(), index);
  if (error != std::errc() || end != name.data() + name.size()) {
    return nullptr;
  }
  return At(index);
}

DocumentEntry* DocumentSet::At(std::size_t index) noexcept {
  return index < entries_.size() ? entries_[index].get() : nullptr;
}

DocumentEntry* DocumentSet::Active() noexcept { return Find(active_); }

const DocumentEntry* DocumentSet::Active() const noexcept {
  return Find(active_);
}

EditDocument* DocumentSet::ActiveDocument() noexcept {
  DocumentEntry* active = Active();
  return active != nullptr ? active->document.get() : nullptr;
}

const EditDocument* DocumentSet::ActiveDocument() const noexcept {
  const DocumentEntry* active = Active();
  return active != nullptr ? active->document.get() : nullptr;
}

std::size_t DocumentSet::ActiveIndex() const noexcept {
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i]->id == active_) return i;
  }
  return entries_.size();
}

std::vector<DocumentEntry*> DocumentSet::Dirty() const {
  std::vector<DocumentEntry*> dirty;
  for (const std::unique_ptr<DocumentEntry>& entry : entries_) {
    if (entry->document->Dirty()) dirty.push_back(entry.get());
  }
  return dirty;
}

void DocumentSet::Clear() noexcept {
  entries_.clear();
  active_ = kNoDocument;
}

}  // namespace se::editor
