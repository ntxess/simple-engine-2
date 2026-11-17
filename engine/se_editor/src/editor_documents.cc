// Copyright 2026 The simple-engine-2 Authors.
//
// The Editor's documents (M10a, M11b; ADR-117): open (or activate), close
// through the close prompt, switch, save and Save All through the lane,
// autosave and recovery per document, and the lane completions of saves.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_assets/lane_jobs.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_editor/documents.h"
#include "se_editor/edit_document.h"
#include "se_editor/editor.h"
#include "se_editor/recovery.h"
#include "se_platform/fs.h"
#include "se_runtime/app.h"
#include "se_runtime/heartbeat.h"
#include "src/editor_internal.h"
#include "src/json_value.h"
#include "src/play_states.h"

namespace se::editor {
namespace {

std::string StemOf(std::string_view path) {
  std::string name(path.substr(path.find_last_of("/\\") + 1));
  for (std::string_view suffix : {".scene.json", ".prefab.json", ".json"}) {
    if (name.ends_with(suffix)) {
      name.resize(name.size() - suffix.size());
      break;
    }
  }
  return name;
}

}  // namespace

std::string Editor::RefOf(std::string_view path) const {
  if (app_ == nullptr) return {};
  const std::string root =
      fs::Normalize(fs::Join(app_->Paths().content_root, game_.asset_dir));
  const std::string normal = fs::Normalize(path);
  if (normal.size() <= root.size() + 1 || !normal.starts_with(root) ||
      normal[root.size()] != '/') {
    return {};
  }
  return normal.substr(root.size() + 1);
}

std::expected<std::unique_ptr<EditDocument>, Error> Editor::LoadDocument(
    App& app, const std::string& path, std::string_view text) {
  DocumentDesc desc;
  desc.path = fs::Normalize(path);
  desc.name = StemOf(desc.path);
  desc.asset_root = fs::Join(app.Paths().content_root, game_.asset_dir);
  // The game's seed (GameSetup::seed through the module's StateEnv).
  desc.seed = 1;
  desc.tick_rate = kDefaultTickRate;
  // The kind by the file's "format" (a prefab is se-prefab-v1).
  if (auto root = json::Parse(text)) {
    const json::Value* format = root->Find("format");
    if (format != nullptr && format->text == "se-prefab-v1") {
      desc.kind = DocumentKind::kPrefab;
      desc.ref = RefOf(desc.path);
      if (desc.ref.empty()) {
        desc.ref = desc.path.substr(desc.path.find_last_of('/') + 1);
      }
    }
  }
  const InlineOwnerScope owner(OnMainThread());
  return EditDocument::Open(app.GameInstance()->Registry(), game_,
                            std::move(desc), text);
}

std::expected<void, Error> Editor::OpenDocument(App& app,
                                                const std::string& path,
                                                bool offer_recovery) {
  AssertMainThread();
  if (play_ || documents_.Frozen()) {
    return Fail(ErrorCode::kInvalidArgument, "Stop before opening a document");
  }
  if (DocumentEntry* open = documents_.FindByPath(path)) {
    ActivateDocument(open->id);
    SE_LOG_INFO("editor: {} is open already: activated",
                open->document->Desc().path);
    return {};
  }
  // A synchronous load on main: known long work to the watchdog (M12).
  const HeartbeatScope beat = app.KnownLongWork("open document");
  auto text = fs::ReadTextFile(path);
  if (!text) return std::unexpected(std::move(text.error()));
  auto document = LoadDocument(app, path, *text);
  if (!document) return std::unexpected(std::move(document.error()));
  edits_.EndScope(documents_, "another document was opened");
  const DocumentId id = documents_.Add(std::move(*document));
  EditDocument& opened = *documents_.Find(id)->document;
  Watch(opened, /*watch=*/true);
  SyncDocumentStack();
  SE_LOG_INFO(
      "editor: opened {} as {} document {} ({} entities, {} prefabs; "
      "{} open)",
      opened.Desc().path, DocumentKindName(opened.Desc().kind), id,
      opened.Get().EntityCount(), opened.PrefabTexts().size(),
      documents_.size());
  if (offer_recovery) OfferRecovery(app, id);
  return {};
}

void Editor::SyncDocumentStack() {
  const DocumentId active = documents_.ActiveId();
  if (active == stack_document_ &&
      (document_states_ || active == kNoDocument)) {
    return;
  }
  DropDocumentStack();
  EditDocument* document = Active();
  if (document == nullptr) return;
  StateEnv env;
  env.registry = &document->Get().Registry();
  env.world_kind = WorldKind::kEditDocument;
  // The state shows the document's World; it never steps.
  SetPendingDocumentWorld(document->StateWorld());
  document_states_ = std::make_unique<GameStateStack>(document_factory_, env);
  const InlineOwnerScope owner(OnMainThread());
  document_states_->Request(StackOp::kPush, kDocumentState);
  document_states_->ApplyPending();
  stack_document_ = active;
}

void Editor::DropDocumentStack() {
  if (document_states_) {
    const InlineOwnerScope owner(OnMainThread());
    document_states_->Clear();
    document_states_.reset();
  }
  stack_document_ = kNoDocument;
}

bool Editor::ActivateDocument(DocumentId id) {
  if (id == documents_.ActiveId()) return true;
  if (documents_.Frozen() || documents_.Find(id) == nullptr) return false;
  edits_.EndScopeOf(documents_, documents_.ActiveId(),
                    "its document was deactivated");
  documents_.Activate(id);
  SyncDocumentStack();
  SE_LOG_INFO("editor: document {} ('{}') is active", id,
              Active()->Desc().name);
  return true;
}

void Editor::CloseDocumentNow(DocumentId id) {
  DocumentEntry* entry = documents_.Find(id);
  if (entry == nullptr || documents_.Frozen()) return;
  edits_.EndScopeOf(documents_, id, "its document was closed");
  Watch(*entry->document, /*watch=*/false);
  const std::string name = entry->document->Desc().name;
  if (stack_document_ == id) DropDocumentStack();
  if (close_prompt_ == id) close_prompt_ = kNoDocument;
  if (close_after_save_ == id) close_after_save_ = kNoDocument;
  if (recovery_document_ == id) {
    recovery_offer_.clear();
    recovery_document_ = kNoDocument;
  }
  std::unique_ptr<EditDocument> closed = documents_.Close(id);
  SyncDocumentStack();
  SE_LOG_INFO("editor: closed document {} ('{}'); {} open", id, name,
              documents_.size());
}

DocumentId Editor::ReplaceDocument(DocumentId id,
                                   std::unique_ptr<EditDocument> document) {
  DocumentEntry* entry = documents_.Find(id);
  if (entry == nullptr || document == nullptr || documents_.Frozen()) {
    return kNoDocument;
  }
  edits_.EndScopeOf(documents_, id, "its document was replaced");
  Watch(*entry->document, /*watch=*/false);
  if (stack_document_ == id) DropDocumentStack();
  const DocumentId next = documents_.Replace(id, std::move(document));
  Watch(*documents_.Find(next)->document, /*watch=*/true);
  SyncDocumentStack();
  return next;
}

void Editor::RequestClose(App& app, DocumentId id) {
  DocumentEntry* entry = documents_.Find(id);
  if (entry == nullptr) return;
  if (play_ || documents_.Frozen()) {
    SE_LOG_WARN("editor: close of '{}' refused: Stop first",
                entry->document->Desc().name);
    return;
  }
  if (!entry->document->Dirty()) {
    CloseDocumentNow(id);
    return;
  }
  close_prompt_ = id;
  SE_LOG_INFO(
      "editor: '{}' has unsaved changes: the close waits for the "
      "prompt",
      entry->document->Desc().name);
  if (!close_answer_.empty()) {
    ResolveClosePrompt(app, std::exchange(close_answer_, {}));
  }
}

void Editor::ResolveClosePrompt(App& app, std::string_view answer) {
  const DocumentId id = std::exchange(close_prompt_, kNoDocument);
  DocumentEntry* entry = documents_.Find(id);
  if (entry == nullptr) return;
  const std::string name = entry->document->Desc().name;
  if (answer == "cancel") {
    SE_LOG_INFO("editor: close of '{}' cancelled: it keeps its changes", name);
    return;
  }
  if (answer == "save" && entry->document->Dirty()) {
    if (auto saved = SaveDocument(app, id, {}, /*durable=*/true); !saved) {
      entry->save_error = saved.error().message;
      save_error_ = entry->save_error;
      SE_LOG_ERROR("editor: close of '{}' called off: the save failed: {}",
                   name, save_error_);
      return;
    }
    close_after_save_ = id;
    return;
  }
  SE_LOG_INFO("editor: close of '{}': its changes are discarded", name);
  CloseDocumentNow(id);
}

void Editor::RunPendingOpen(App& app) {
  if (close_after_save_ != kNoDocument) {
    const DocumentId id = close_after_save_;
    const bool saving = std::any_of(
        saves_.begin(), saves_.end(), [id](const PendingSave& save) {
          return !save.recovery && save.document == id;
        });
    if (!saving) {
      close_after_save_ = kNoDocument;
      DocumentEntry* entry = documents_.Find(id);
      if (entry != nullptr && entry->document->Dirty()) {
        SE_LOG_WARN("editor: close of '{}' called off: it is still unsaved{}",
                    entry->document->Desc().name,
                    entry->save_error.empty()
                        ? std::string(" (changed since the save)")
                        : " (" + entry->save_error + ")");
      } else if (entry != nullptr) {
        CloseDocumentNow(id);
      }
    }
  }
  if (!pending_open_.has_value()) return;
  std::string path = std::move(*pending_open_);
  pending_open_.reset();
  if (auto opened = OpenDocument(app, path, /*offer_recovery=*/true); !opened) {
    SE_LOG_ERROR("editor: open {}: {}", path, opened.error().message);
  }
}

// --- Recovery ---

RecoveryFile Editor::RecoveryOf(const EditDocument& document,
                                std::string text) const {
  RecoveryFile file;
  file.document = document.Desc().path;
  file.name = document.Desc().name;
  file.revision = document.Revision();
  file.kind = document.Desc().kind;
  file.ref = document.Desc().ref;
  file.scene = std::move(text);
  if (app_ != nullptr) {
    file.path = RecoveryPathFor(app_->Paths().pref, file.document, file.name);
  }
  return file;
}

void Editor::OfferRecovery(App& app, DocumentId id) {
  const DocumentEntry* entry = documents_.Find(id);
  if (entry == nullptr) return;
  std::vector<RecoveryFile> offer;
  for (RecoveryFile& file : FindRecoveryFiles(app.Paths().pref)) {
    if (fs::Normalize(file.document) == entry->document->Desc().path) {
      SE_LOG_INFO("editor: recovery file {} (revision {}) for {}", file.path,
                  file.revision, file.document);
      offer.push_back(std::move(file));
    }
  }
  if (offer.empty()) return;
  recovery_offer_ = std::move(offer);
  recovery_document_ = id;
  if (!recovery_answer_.empty()) ResolveRecovery(app, recovery_answer_);
}

void Editor::ResolveRecovery(App& app, std::string_view answer) {
  if (recovery_offer_.empty()) return;
  RecoveryFile file = std::move(recovery_offer_.front());
  recovery_offer_.clear();
  const DocumentId id = std::exchange(recovery_document_, kNoDocument);
  DocumentEntry* entry = documents_.Find(id);
  if (answer == "accept" && entry != nullptr && !play_) {
    auto recovered =
        LoadDocument(app, entry->document->Desc().path, file.scene);
    if (!recovered) {
      SE_LOG_ERROR("editor: recovery {}: {}", file.path,
                   recovered.error().message);
      return;
    }
    // The file on disk is what it was: the recovered changes are unsaved.
    (*recovered)->SetFileText(entry->document->FileText());
    (*recovered)->MarkSaved(~std::uint64_t{0});
    const DocumentId next = ReplaceDocument(id, std::move(*recovered));
    SE_LOG_INFO("editor: recovered {} from {} (revision {}) as document {}",
                file.document, file.path, file.revision, next);
    return;
  }
  if (auto removed = fs::Remove(file.path); !removed) {
    SE_LOG_WARN("editor: {}: {}", file.path, removed.error().message);
  }
  SE_LOG_INFO("editor: recovery file {} discarded", file.path);
}

// --- Saves ---

std::expected<void, Error> Editor::SaveDocument(App& app, DocumentId id,
                                                const std::string& path,
                                                bool durable) {
  DocumentEntry* entry = documents_.Find(id);
  if (entry == nullptr) return Fail(ErrorCode::kInvalidArgument, "no document");
  EditDocument& document = *entry->document;
  edits_.EndScopeOf(documents_, id, "its document is saved");
  document.EndCoalesce();
  if (!path.empty() && fs::Normalize(path) != document.Desc().path) {
    // Save As: the watch follows the file.
    Watch(document, /*watch=*/false);
    document.SetPath(fs::Normalize(path));
    Watch(document, /*watch=*/true);
  }
  if (document.Desc().path.empty()) {
    return Fail(ErrorCode::kInvalidArgument, "the document has no file");
  }
  auto text = document.SerializeFile();
  if (!text) return std::unexpected(std::move(text.error()));
  const Ticket ticket =
      SubmitWrite(app, document.Desc().path, AsBytes(*text), durable);
  if (ticket == 0) {
    return Fail(ErrorCode::kIo, "the background lane took no save");
  }
  PendingSave save;
  save.ticket = ticket;
  save.revision = document.Revision();
  save.path = document.Desc().path;
  save.durable = durable;
  save.document = id;
  save.text = std::move(*text);
  saves_.push_back(std::move(save));
  entry->save_error.clear();
  save_error_.clear();
  SE_LOG_INFO("editor: saving {} (revision {}) through the lane",
              document.Desc().path, document.Revision());
  return {};
}

bool Editor::SaveAll(App& app) {
  bool ok = true;
  for (DocumentEntry* entry : documents_.Dirty()) {
    if (auto saved = SaveDocument(app, entry->id, {}, /*durable=*/true);
        !saved) {
      entry->save_error = saved.error().message;
      save_error_ = std::format("{}: {}", entry->document->Desc().name,
                                entry->save_error);
      SE_LOG_ERROR("editor: save of '{}' failed: {}",
                   entry->document->Desc().name, entry->save_error);
      ok = false;
    }
  }
  return ok;
}

void Editor::Autosave(App& app, std::uint64_t now_ns) {
  for (const auto& owned : documents_.All()) {
    DocumentEntry& entry = *owned;
    EditDocument& document = *entry.document;
    // No input since: the document's activity is its edits too.
    const std::uint64_t quiet_since =
        std::max(last_input_ns_, entry.last_change_ns);
    if (document.Revision() != entry.seen_revision) {
      entry.seen_revision = document.Revision();
      entry.last_change_ns = now_ns;
      continue;
    }
    if (!entry.autosave.Due(now_ns, quiet_since, document.Revision(),
                            document.Dirty())) {
      continue;
    }
    auto text = document.SerializeFile();
    if (!text) {
      SE_LOG_ERROR("editor: autosave of '{}': {}", document.Desc().name,
                   text.error().message);
      entry.autosave.Submitted(document.Revision());
      continue;
    }
    RecoveryFile file = RecoveryOf(document, std::move(*text));
    const Ticket ticket =
        SubmitWrite(app, file.path, AsBytes(EncodeRecovery(file)),
                    /*durable=*/false);
    entry.autosave.Submitted(document.Revision());
    if (ticket != 0) {
      PendingSave save;
      save.ticket = ticket;
      save.revision = document.Revision();
      save.path = file.path;
      save.recovery = true;
      save.document = entry.id;
      saves_.push_back(std::move(save));
    }
    SE_LOG_INFO("editor: autosave of {} (revision {}) to {}", file.document,
                file.revision, file.path);
  }
}

void Editor::WriteRecoveryFiles(QuitReason reason) {
  AssertMainThread();
  if (app_ == nullptr) return;
  for (DocumentEntry* entry : documents_.Dirty()) {
    const EditDocument& document = *entry->document;
    auto text = document.SerializeFile();
    if (!text) {
      SE_LOG_ERROR("editor: recovery of '{}' at {}: {}", document.Desc().name,
                   QuitReasonName(reason), text.error().message);
      continue;
    }
    const RecoveryFile file = RecoveryOf(document, std::move(*text));
    if (auto written = WriteRecoveryNow(file); !written) {
      SE_LOG_ERROR("editor: recovery file {}: {}", file.path,
                   written.error().message);
      continue;
    }
    SE_LOG_WARN("editor: {}: recovery file {} written (revision {})",
                QuitReasonName(reason), file.path, file.revision);
  }
}

void Editor::OnLaneCompletion(const LaneCompletion& completion) {
  // Hot reload: a watched file changed, or a read of one completed.
  if (completion.result) {
    if (const auto* change = std::get_if<FileChange>(&*completion.result)) {
      // Handled at the frame start (RunFileEvents).
      if (change->tag == kEditorWatchTag) {
        file_events_.push_back(FileEvent{change->path, false, {}});
        if (play_) {
          // Gameplay assets are frozen until Stop (M10a).
          SE_LOG_INFO("editor: {} changed during Play; handled at Stop",
                      change->path);
        }
      }
      return;
    }
  }
  if (const auto read = std::find_if(reads_.begin(), reads_.end(),
                                     [&](const auto& pending) {
                                       return pending.first ==
                                              completion.ticket;
                                     });
      read != reads_.end()) {
    const std::string path = std::move(read->second);
    reads_.erase(read);
    if (!completion.result) {
      ++file_events_handled_;
      SE_LOG_ERROR("editor: reading {} after its change: {}", path,
                   completion.result.error().message);
      return;
    }
    const auto* bytes =
        std::get_if<std::vector<std::byte>>(&*completion.result);
    std::string text;
    if (bytes != nullptr) {
      text.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    }
    file_events_.push_back(FileEvent{path, true, std::move(text)});
    return;
  }
  const auto it =
      std::find_if(saves_.begin(), saves_.end(), [&](const PendingSave& save) {
        return save.ticket == completion.ticket;
      });
  if (it == saves_.end()) return;
  const PendingSave save = std::move(*it);
  saves_.erase(it);
  if (!completion.result) {
    const Error& error = completion.result.error();
    if (!save.recovery) {
      // A failed save keeps the document dirty and shows its error; a quit
      // waiting on it is called off.
      if (DocumentEntry* entry = documents_.Find(save.document)) {
        entry->save_error = error.message;
      }
      save_error_ = std::format("{}: {}", save.path, error.message);
      if (quit_phase_ == QuitPhase::kSaving) quit_cancel_ = true;
    }
    SE_LOG_ERROR("editor: {} of {} failed: {}",
                 save.recovery ? "autosave" : "save", save.path, error.message);
    return;
  }
  SaveCompleted(save);
}

void Editor::SaveCompleted(const PendingSave& save) {
  DocumentEntry* entry = documents_.Find(save.document);
  // The dock layout, or a document closed since.
  if (entry == nullptr || app_ == nullptr) return;
  EditDocument& document = *entry->document;
  const std::string recovery = RecoveryPathFor(
      app_->Paths().pref, document.Desc().path, document.Desc().name);
  const auto remove_recovery = [&recovery] {
    if (!fs::Exists(recovery)) return;
    if (auto removed = fs::Remove(recovery); !removed) {
      SE_LOG_WARN("editor: {}: {}", recovery, removed.error().message);
    }
  };
  if (save.recovery) {
    // An autosave that lands after a save of the same or a newer revision
    // completed holds older content than the file: it is no recovery.
    if (save.path == recovery && entry->last_save_revision.has_value() &&
        save.revision <= *entry->last_save_revision) {
      remove_recovery();
      SE_LOG_INFO(
          "editor: autosave of revision {} landed after the save of "
          "revision {}: recovery file removed",
          save.revision, *entry->last_save_revision);
    }
    return;
  }
  if (save.path != document.Desc().path) return;
  document.MarkSaved(save.revision);
  document.SetFileText(save.text);
  entry->last_save_revision =
      std::max(entry->last_save_revision.value_or(0), save.revision);
  // A saved document needs no recovery file.
  remove_recovery();
  SE_LOG_INFO("editor: saved {} (revision {}){}", save.path, save.revision,
              document.Dirty() ? "; changed since" : "");
  if (document.Desc().kind == DocumentKind::kPrefab) {
    // A saved prefab: the open scenes follow at once, at this frame's
    // start (the scan's later report of the same content changes nothing).
    file_events_.push_back(FileEvent{save.path, true, save.text});
  }
}

}  // namespace se::editor
