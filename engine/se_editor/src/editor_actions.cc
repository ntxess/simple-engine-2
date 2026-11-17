// Copyright 2026 The simple-engine-2 Authors.
//
// The Editor's actions: dialog results, Play requests and the two-phase
// quit. Documents are in editor_documents.cc, hot reload in
// editor_reload.cc, the test script in editor_script.cc.

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "se_assets/lane_jobs.h"
#include "se_assets/payloads.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_editor/commands.h"
#include "se_editor/edit_document.h"
#include "se_editor/editor.h"
#include "se_editor/editor_script.h"
#include "se_editor/editor_stats.h"
#include "se_editor/play_session.h"
#include "se_editor/recovery.h"
#include "se_os/thread.h"
#include "se_platform/fs.h"
#include "se_platform/fs_faults.h"
#include "se_platform/window.h"
#include "se_runtime/action_map.h"
#include "se_runtime/app.h"
#include "se_runtime/asset_runtime.h"
#include "se_runtime/heartbeat.h"
#include "src/editor_internal.h"
#include "src/json_value.h"
#include "src/play_states.h"

namespace se::editor {

void Editor::TakeDialogs(App& app) {
  for (DialogResult& result : dialogs_->Take()) {
    if (!result.error.empty()) {
      SE_LOG_ERROR("editor: file dialog {}: {}", result.id, result.error);
      continue;
    }
    if (result.cancelled || result.paths.empty()) {
      if (app.Quit().Pending()) ++dialogs_during_quit_;
      SE_LOG_INFO("editor: file dialog {} cancelled{}", result.id,
                  app.Quit().Pending() ? " during the quit" : "");
      continue;
    }
    // Nothing acts on a dialog once the editor started quitting.
    if (app.Quit().Pending()) {
      ++dialogs_during_quit_;
      SE_LOG_INFO("editor: file dialog {} answered during the quit: ignored",
                  result.id);
      continue;
    }
    if (result.kind == DialogKind::kOpen) {
      pending_open_ = result.paths.front();
      SE_LOG_INFO("editor: file dialog {}: open {}", result.id,
                  result.paths.front());
    } else if (auto saved = SaveDocument(app, documents_.ActiveId(),
                                         result.paths.front(),
                                         /*durable=*/true);
               !saved) {
      save_error_ = saved.error().message;
    }
  }
}

// --- Play ---

void Editor::RunPlayRequest(App& app, std::uint64_t /*now_ns*/) {
  const Request request = std::exchange(request_, Request::kNone);
  switch (request) {
    case Request::kNone:
      return;
    case Request::kPlay:
      if (!play_) {
        StartPlay(app, PlayKind::kPlay);
      } else {
        ResumePlay();
      }
      return;
    case Request::kPlayGame:
      if (!play_) StartPlay(app, PlayKind::kPlayGame);
      return;
    case Request::kPause:
      if (play_ && play_->State() == PlayState::kPlaying) {
        play_->Pause();
        SE_LOG_INFO("play: paused before tick {}", play_->NextTick());
      }
      return;
    case Request::kResume:
      if (play_) ResumePlay();
      return;
    case Request::kStep:
      if (play_ && play_->State() == PlayState::kPaused) {
        play_->Step();
        SE_LOG_INFO("play: step to tick {}", play_->NextTick());
      }
      return;
    case Request::kStop:
      if (play_) {
        // The World goes at the next frame start (this frame's ImDrawData
        // may still name its viewports' textures).
        stop_pending_ = true;
        SE_LOG_INFO("play: stop after tick {}", play_->LastTick());
      }
      return;
  }
}

void Editor::ResumePlay() {
  // Resume acts only on Paused and Stepping (not on Play.Faulted).
  const EditorMode before = Mode();
  play_->Resume();
  if (Mode() != before) {
    SE_LOG_INFO("play: resumed at tick {}", play_->NextTick());
  } else {
    SE_LOG_INFO("play: resume ignored in {}", EditorModeName(before));
  }
}

void Editor::StartPlay(App& app, PlayKind kind) {
  EditDocument* document = Active();
  if (play_ || document == nullptr) return;
  if (document->Desc().kind != DocumentKind::kScene) {
    SE_LOG_WARN("play: '{}' is a prefab document; Play plays a scene",
                document->Desc().name);
    return;
  }
  // No scope outlives Play's start, and nothing queued for the documents
  // applies while it runs.
  edits_.EndScope(documents_, "Play started");
  document->EndCoalesce();
  // The Play-start bytes stay flattened: a replay or a dump never depends
  // on prefab files that may change after it was written.
  auto text = document->Serialize();
  if (!text) {
    SE_LOG_ERROR("play: the document did not serialize: {}",
                 text.error().message);
    return;
  }
  PlayStart start;
  start.kind = kind;
  start.bytes = AsBytes(*text);
  start.document_hash = document->Hash();
  start.asset_root = document->Desc().asset_root;
  start.seed = document->Desc().seed;
  start.tick_rate = document->Desc().tick_rate;
  if (kind == PlayKind::kPlayGame) {
    // The start state loads the document from a file.
    start.scene_path = app.Paths().pref + "play-game.scene.json";
    if (auto written = fs::WriteFileAtomic(start.scene_path, start.bytes);
        !written) {
      SE_LOG_ERROR("play game: {}: {}", start.scene_path,
                   written.error().message);
      return;
    }
  }
  const std::vector<std::byte> start_bytes = start.bytes;
  std::expected<std::unique_ptr<PlaySession>, Error> session;
  {
    // The serializer round trip into the play World, on main: known long
    // work to the watchdog (M12).
    const HeartbeatScope beat = app.KnownLongWork("play start");
    // 'Play game' loads its states through the content gate and reads
    // their texts from the lane, as the runtime does (ADR-129).
    AssetRuntime* content =
        kind == PlayKind::kPlayGame ? app.Assets() : nullptr;
    session =
        PlaySession::Start(*app.GameInstance(), *app.GamePool(), game_,
                           std::move(start), app.Assets(), content, content);
  }
  if (!session) {
    SE_LOG_ERROR("play: {}", session.error().message);
    return;
  }
  play_ = std::move(*session);
  ++play_count_;
  // The documents are frozen while a Play session exists.
  play_document_ = documents_.ActiveId();
  documents_.SetFrozen(true);
  play_start_content_hash_ = document->ContentHash();
  AttachPlayDump();
  // Gameplay assets freeze until Stop; visual swaps go to the SessionLog.
  if (AssetRuntime* assets = app.Assets()) {
    assets->Registry().SetGameplayFrozen(true);
    assets->Registry().SetSwapListener(
        [this](std::string_view key, std::uint64_t hash) {
          // The registry applies swaps on main.
          AssertMainThread();
          if (play_ && !play_->Faulted()) play_->LogAssetSwap(key, hash);
        });
  }
  play_start_document_hash_ = play_->DocumentHash();
  render_discarded_ = false;
  fault_directory_.clear();
  // The Play-start bytes: a recovery file through the lane, and kept by
  // the session (its replay's start).
  SubmitWrite(app,
              RecoveryDirectory(app.Paths().pref) + "play-start.scene.json",
              start_bytes, /*durable=*/false);
  SE_LOG_INFO("{}: started at tick {} ({} bytes, document hash {:016x})",
              kind == PlayKind::kPlayGame ? "play game" : "play",
              play_->NextTick(), start_bytes.size(), play_start_document_hash_);
  // 'Play game' plays in the view: its window takes the focus, so the
  // keys are the game's at once (ADR-113).
  if (kind == PlayKind::kPlayGame && !viewports_->ReportingPanel().empty()) {
    focus_queue_.push_back(viewports_->ReportingPanel());
  }
}

void Editor::StopPlay(App& app) {
  stop_pending_ = false;
  if (!play_) return;
  const Tick last = play_->LastTick();
  const bool faulted = play_->Faulted();
  std::vector<std::byte> replay = play_->FinishReplay();
  if (!replay.empty()) {
    SubmitWrite(app, app.Paths().pref + "sessions/last-play.sereplay",
                std::move(replay), /*durable=*/true);
  }
  // The dump streams describe no session from here on.
  DetachPlayDump();
  // Its voices stop with its World (the AudioEvents' scope).
  play_.reset();
  // Gameplay assets changed during Play reload now.
  if (AssetRuntime* assets = app.Assets()) {
    assets->Registry().SetSwapListener({});
    assets->Registry().SetGameplayFrozen(false);
  }
  render_discarded_ = false;
  game_input_focus_ = false;
  documents_.SetFrozen(false);
  const DocumentEntry* played = documents_.Find(play_document_);
  play_document_ = kNoDocument;
  const std::uint64_t now = played ? played->document->Hash() : 0;
  const std::uint64_t content = played ? played->document->ContentHash() : 0;
  SE_ASSERT_MSG(now == play_start_document_hash_,
                "Stop: the document's hash changed during Play");
  SE_ASSERT_MSG(content == play_start_content_hash_,
                "Stop: the document's content hash changed during Play");
  if (now != play_start_document_hash_ || content != play_start_content_hash_) {
    SE_LOG_ERROR(
        "stop{} after tick {}: the document changed during Play (hash "
        "{:016x}, {:016x} at Play; content {:016x}, {:016x} at Play)",
        faulted ? " (from Play.Faulted)" : "", last, now,
        play_start_document_hash_, content, play_start_content_hash_);
  } else {
    SE_LOG_INFO(
        "stop{} after tick {}: document hash {:016x} and content hash "
        "{:016x} unchanged",
        faulted ? " (from Play.Faulted)" : "", last, now, content);
  }
  // The file changes kept during Play run at this frame's start, in order
  // (RunFileEvents, after this).
}

// --- Quit ---

QuitPoll Editor::PollQuit(QuitReason /*reason*/) {
  // The QuitController polls its providers on main.
  AssertMainThread();
  // A file dialog is open: its callback first.
  if (dialogs_ && dialogs_->Pending() > 0) return QuitPoll::kPending;
  if (quit_cancel_) return QuitPoll::kCancel;
  // A close waiting for its prompt or its save does not survive the quit:
  // the quit's own prompt lists every dirty document.
  if (close_prompt_ != kNoDocument || close_after_save_ != kNoDocument) {
    SE_LOG_INFO(
        "editor: the close of document {} is dropped: the editor is "
        "quitting",
        close_prompt_ != kNoDocument ? close_prompt_ : close_after_save_);
    close_prompt_ = kNoDocument;
    close_after_save_ = kNoDocument;
  }
  switch (quit_phase_) {
    case QuitPhase::kNone:
      if (!documents_.Dirty().empty()) {
        quit_phase_ = QuitPhase::kPrompt;
        if (!quit_answer_.empty() && app_ != nullptr) {
          ResolveQuitPrompt(*app_, quit_answer_);
        }
        return quit_cancel_ ? QuitPoll::kCancel : QuitPoll::kPending;
      }
      if (app_ != nullptr) ResolveQuitPrompt(*app_, "discard");
      return QuitPoll::kPending;
    case QuitPhase::kPrompt:
      return QuitPoll::kPending;
    case QuitPhase::kSaving: {
      const bool waiting =
          std::any_of(saves_.begin(), saves_.end(),
                      [](const PendingSave& save) { return save.durable; });
      if (waiting) return QuitPoll::kPending;
      quit_phase_ = QuitPhase::kReady;
      return QuitPoll::kReady;
    }
    case QuitPhase::kReady:
      return QuitPoll::kReady;
  }
  return QuitPoll::kReady;
}

void Editor::QuitCancelled() {
  if (quit_phase_ != QuitPhase::kNone) {
    SE_LOG_INFO("editor: quit cancelled{}",
                save_error_.empty() ? std::string()
                                    : " (save failed: " + save_error_ + ")");
  }
  quit_phase_ = QuitPhase::kNone;
  quit_cancel_ = false;
  quit_answer_.clear();
}

void Editor::ResolveQuitPrompt(App& app, std::string_view answer) {
  if (answer == "cancel") {
    quit_cancel_ = true;
    return;
  }
  if (answer == "save" && !documents_.Dirty().empty()) {
    // Save All: each dirty document as a Durable job. One that cannot be
    // submitted keeps its document dirty with its error and calls the quit
    // off; the others are saved.
    if (!SaveAll(app)) quit_cancel_ = true;
  }
  // The dock layout, as a Durable job too.
  if (imgui_) {
    const std::string layout = imgui_->SaveSettings();
    const Ticket ticket =
        SubmitWrite(app, layout_path_, AsBytes(layout), /*durable=*/true);
    if (ticket != 0) {
      saves_.push_back(PendingSave{ticket, 0, layout_path_, true, true});
    }
  }
  quit_phase_ = QuitPhase::kSaving;
}

}  // namespace se::editor
