// Copyright 2026 The simple-engine-2 Authors.
//
// Hot reload into documents (docs/milestone-specs.md M11b "Hot reload into
// documents"; ADR-120). Every open document watches its own file and the
// prefab files its load reported (BackgroundLane::Watch with the editor's
// tag; the scan's FileChange reaches the editor through the AssetRuntime's
// foreign handler). A change is read on the lane (an Urgent read job) and
// compared by content with the text the document holds:
//   - a prefab that changed: every open scene holding instances of it gets
//     one undo step "Prefab changed: <ref>" (RebaseDocument);
//   - a document's own file: unchanged content is ignored, a clean
//     document is reloaded (its undo stack starts again), a dirty one asks
//     (Reload, Keep mine).
// The scan's baseline is the file as it is when the watch is added on the
// lane, which is after the document read it; so the first watch of a path
// is followed, on the path's strand, by a catch-up read compared like any
// change: a file written between the document's read and the watch is
// still seen.
// During Play the changes wait for Stop and are handled then, in order.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_assets/background_lane.h"
#include "se_assets/lane_jobs.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_editor/documents.h"
#include "se_editor/edit_document.h"
#include "se_editor/editor.h"
#include "se_editor/prefab_commands.h"
#include "se_platform/fs.h"
#include "se_runtime/app.h"
#include "src/editor_internal.h"

namespace se::editor {

std::vector<std::string> Editor::WatchedPaths(
    const EditDocument& document) const {
  std::vector<std::string> paths;
  if (!document.Desc().path.empty()) paths.push_back(document.Desc().path);
  for (const auto& [ref, text] : document.PrefabTexts()) {
    paths.push_back(fs::Normalize(fs::Join(document.Desc().asset_root, ref)));
  }
  return paths;
}

void Editor::Watch(const EditDocument& document, bool watch) {
  AssetRuntime* assets = app_ != nullptr ? app_->Assets() : nullptr;
  for (const std::string& path : WatchedPaths(document)) {
    int& count = watch_refs_[path];
    if (watch) {
      if (count++ == 0 && assets != nullptr) {
        JobDesc desc;
        desc.strand = StrandOf(path);
        assets->Lane().Submit(desc, MakeWatchJob(path, kEditorWatchTag, true));
        const Ticket ticket = assets->Lane().Submit(desc, MakeReadJob(path));
        reads_.emplace_back(ticket, path);
        catch_up_reads_.push_back(ticket);
      }
      continue;
    }
    if (count == 0) continue;
    --count;
    if (count == 0) {
      watch_refs_.erase(path);
      if (assets != nullptr) {
        assets->Lane().Submit(JobDesc{},
                              MakeWatchJob(path, kEditorWatchTag, false));
      }
    }
  }
}

void Editor::OnFileChanged(const FileChange& change) {
  if (!watch_refs_.contains(fs::Normalize(change.path))) return;
  if (!change.exists) {
    ++file_events_handled_;
    SE_LOG_WARN("editor: {} was removed; the documents keep what they hold",
                change.path);
    return;
  }
  if (play_) {
    // Gameplay assets are frozen until Stop (M10a): read then, in order.
    file_events_.push_back(FileEvent{change.path, false, {}});
    SE_LOG_INFO("editor: {} changed during Play; handled at Stop", change.path);
    return;
  }
  AssetRuntime* assets = app_ != nullptr ? app_->Assets() : nullptr;
  if (assets == nullptr) return;
  JobDesc desc;
  desc.job_class = JobClass::kUrgent;
  const Ticket ticket = assets->Lane().Submit(desc, MakeReadJob(change.path));
  reads_.emplace_back(ticket, fs::Normalize(change.path));
  SE_LOG_INFO("editor: {} changed on disk; reading it", change.path);
}

void Editor::OnFileRead(const std::string& path, std::string text,
                        bool catch_up) {
  if (play_) {
    file_events_.push_back(FileEvent{path, true, std::move(text), catch_up});
    return;
  }
  bool differs = false;
  const std::string normal = fs::Normalize(path);
  // The document open on this file.
  if (DocumentEntry* entry = documents_.FindByPath(normal)) {
    const DocumentId id = entry->id;
    const FileTextAction action = ClassifyFileText(*entry->document, text);
    differs = action != FileTextAction::kUnchanged;
    switch (action) {
      case FileTextAction::kUnchanged:
        SE_LOG_DEBUG("editor: {} holds what '{}' has: nothing to reload",
                     normal, entry->document->Desc().name);
        break;
      case FileTextAction::kReload: {
        std::expected<std::unique_ptr<EditDocument>, Error> reopened;
        {
          const InlineOwnerScope owner(OnMainThread());
          reopened = ReopenDocument(*entry->document, game_, text);
        }
        if (!reopened) {
          SE_LOG_ERROR(
              "editor: reloading {}: {} (the document keeps what it "
              "holds)",
              normal, reopened.error().message);
          break;
        }
        const DocumentId next = ReplaceDocument(id, std::move(*reopened));
        SE_LOG_INFO(
            "editor: {} changed on disk: reloaded as document {} (its undo "
            "stack starts again)",
            normal, next);
        break;
      }
      case FileTextAction::kAsk:
        entry->reload_prompt = true;
        entry->reload_text = text;
        SE_LOG_INFO(
            "editor: {} changed on disk while '{}' has unsaved "
            "changes: asking (Reload, Keep mine)",
            normal, entry->document->Desc().name);
        if (!reload_answer_.empty() && app_ != nullptr) {
          ResolveReloadPrompt(*app_, std::exchange(reload_answer_, {}));
        }
        break;
    }
  }
  // A prefab file: the open scenes holding instances of it.
  const std::string ref = RefOf(normal);
  if (!ref.empty() && RebasePrefab(ref, text)) differs = true;
  // Only a read that found something new counts as a handled change, so a
  // second report of one write (the catch-up read and the scan) is not
  // counted twice.
  if (!differs) return;
  if (catch_up) {
    SE_LOG_INFO("editor: {} changed before it was watched; handled", normal);
  }
  ++file_events_handled_;
}

bool Editor::RebasePrefab(const std::string& ref, const std::string& text) {
  bool differs = false;
  for (const auto& owned : documents_.All()) {
    DocumentEntry& entry = *owned;
    EditDocument& document = *entry.document;
    const auto recorded = document.PrefabTexts().find(ref);
    if (recorded == document.PrefabTexts().end() || recorded->second == text) {
      continue;
    }
    differs = true;
    edits_.EndScopeOf(documents_, entry.id, "a prefab it uses changed");
    std::expected<bool, Error> rebased;
    {
      const InlineOwnerScope owner(OnMainThread());
      rebased = RebaseDocument(document, game_, ref, text);
    }
    if (!rebased) {
      SE_LOG_ERROR("editor: prefab {} changed; '{}' was not rebased: {}", ref,
                   document.Desc().name, rebased.error().message);
      continue;
    }
    SE_LOG_INFO("editor: prefab {} changed: '{}' {} (undo: '{}')", ref,
                document.Desc().name,
                *rebased ? "follows it in one undo step"
                         : "has no instance that changes",
                document.UndoLabel());
  }
  return differs;
}

void Editor::ResolveReloadPrompt(App& /*app*/, std::string_view answer) {
  DocumentEntry* asking = nullptr;
  for (const auto& entry : documents_.All()) {
    if (entry->reload_prompt) {
      asking = entry.get();
      break;
    }
  }
  if (asking == nullptr) {
    // The answer for the next prompt.
    reload_answer_ = std::string(answer);
    return;
  }
  asking->reload_prompt = false;
  std::string text = std::move(asking->reload_text);
  asking->reload_text.clear();
  if (answer == "keep") {
    // The file's text is the new one; the document stays dirty, so the
    // next save wins.
    asking->document->SetFileText(std::move(text));
    SE_LOG_INFO("editor: '{}' keeps its changes; the next save wins",
                asking->document->Desc().name);
    return;
  }
  std::expected<std::unique_ptr<EditDocument>, Error> reopened;
  {
    const InlineOwnerScope owner(OnMainThread());
    reopened = ReopenDocument(*asking->document, game_, text);
  }
  if (!reopened) {
    SE_LOG_ERROR("editor: reloading '{}': {}", asking->document->Desc().name,
                 reopened.error().message);
    return;
  }
  const std::string name = asking->document->Desc().name;
  const DocumentId next = ReplaceDocument(asking->id, std::move(*reopened));
  SE_LOG_INFO(
      "editor: '{}' reloaded from its file as document {}; its "
      "changes are discarded",
      name, next);
}

void Editor::RunFileEvents() {
  if (play_ || stop_pending_ || file_events_.empty()) return;
  std::deque<FileEvent> events = std::move(file_events_);
  file_events_.clear();
  for (FileEvent& event : events) {
    if (event.read) {
      OnFileRead(event.path, std::move(event.text), event.catch_up);
      continue;
    }
    FileChange change;
    change.path = event.path;
    change.tag = kEditorWatchTag;
    change.exists = true;
    OnFileChanged(change);
  }
}

}  // namespace se::editor
