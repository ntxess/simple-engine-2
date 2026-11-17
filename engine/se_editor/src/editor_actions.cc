// Copyright 2026 The simple-engine-2 Authors.
//
// The Editor's actions: documents (open, save, recovery, autosave), Play
// requests, dialog results, the two-phase quit, and the test script.

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
#include "se_runtime/heartbeat.h"
#include "src/editor_internal.h"
#include "src/json_value.h"
#include "src/play_states.h"

namespace se::editor {

namespace {

constexpr std::uint64_t kScriptWaitLimitNs = 60'000'000'000;

std::string StemOf(std::string_view path) {
  std::string name(path.substr(path.find_last_of("/\\") + 1));
  for (std::string_view suffix : {".scene.json", ".json"}) {
    if (name.ends_with(suffix)) {
      name.resize(name.size() - suffix.size());
      break;
    }
  }
  return name;
}

std::optional<std::uint64_t> ParseU64(std::string_view text, int base = 10) {
  std::uint64_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value, base);
  if (error != std::errc() || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::optional<float> ParseFloat(std::string_view text) {
  float value = 0.0F;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

bool YesNo(const ScriptAction& action, bool fallback) {
  if (action.args.empty()) return fallback;
  return action.args[0] == "yes" || action.args[0] == "true";
}

// Verbs, the words they take, and whether the last word is the rest of the
// line (JSON).
struct VerbSpec {
  std::string_view verb;
  std::size_t min_args;
  std::size_t max_args;
  bool rest_of_line;
};

constexpr VerbSpec kVerbs[] = {
    {"open", 1, 1, false},
    {"select", 0, 64, false},
    {"set", 3, 3, true},
    {"add", 3, 3, true},
    {"remove", 2, 2, false},
    {"create", 2, 2, true},
    {"create-copy", 2, 2, false},
    {"destroy", 1, 1, false},
    {"path-insert", 4, 4, false},
    {"path-move", 4, 4, false},
    {"path-delete", 2, 2, false},
    {"coalesce-begin", 1, 1, true},
    {"coalesce-end", 0, 0, false},
    {"undo", 0, 0, false},
    {"redo", 0, 0, false},
    {"save", 0, 1, false},
    {"play", 0, 0, false},
    {"play-game", 0, 0, false},
    {"pause", 0, 0, false},
    {"resume", 0, 0, false},
    {"stop", 0, 0, false},
    {"step", 0, 1, false},
    {"inject-throw", 1, 1, false},
    {"wait-ticks", 1, 1, false},
    {"wait-frames", 1, 1, false},
    {"dialog-open", 1, 1, false},
    {"dialog-callback", 1, 2, false},
    {"quit", 0, 1, false},
    {"recovery", 1, 1, false},
    {"expect-hash", 2, 2, false},
    {"expect-faulted", 0, 1, false},
    {"expect-dirty", 0, 1, false},
    {"expect-state", 1, 1, false},
    {"dump-hash", 1, 1, false},
    {"throttle-disk", 1, 1, false},
    {"disk-busy", 1, 1, false},
    {"wait-ms", 1, 1, false},
    {"expect-entities", 2, 2, false},
    {"expect-recovery", 0, 1, false},
    {"expect-quit-pending", 0, 1, false},
    {"terminate", 0, 0, false},
    {"key", 1, 1, false},
    {"push-key", 1, 2, false},
    {"answer-open", 1, 1, false},
    {"expect-open-prompt", 0, 1, false},
    {"wait-saves", 0, 0, false},
    {"wait-autosave", 0, 0, false},
    {"expect-game-keyboard", 0, 1, false},
    {"show-panel", 1, 1, true},
    {"expect-panel", 1, 1, true},
    {"expect-viewport", 0, 1, false},
    {"expect-fork-sites", 1, 1, false},
    {"expect-dump-stream", 0, 1, false},
};

// "ctrl+z", "shift+f5", "f10": modifiers, then one of ImGui's named keys
// (ImGui::GetKeyName, any letter case). 0: not a chord.
ImGuiKeyChord ParseKeyChord(std::string_view text) {
  auto lower = [](std::string_view word) {
    std::string out(word);
    for (char& ch : out) {
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
  };
  ImGuiKeyChord chord = 0;
  while (true) {
    const std::size_t plus = text.find('+');
    const std::string word = lower(text.substr(0, plus));
    if (plus == std::string_view::npos) {
      for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END;
           ++key) {
        if (lower(ImGui::GetKeyName(static_cast<ImGuiKey>(key))) == word) {
          return chord | key;
        }
      }
      return 0;
    }
    if (word == "ctrl") {
      chord |= ImGuiMod_Ctrl;
    } else if (word == "shift") {
      chord |= ImGuiMod_Shift;
    } else if (word == "alt") {
      chord |= ImGuiMod_Alt;
    } else {
      return 0;
    }
    text = text.substr(plus + 1);
  }
}

// The chord's key events for ImGui's queue (never OS input): modifiers
// down before the key, up after it.
void FeedKeyChord(ImGuiKeyChord chord, bool down) {
  ImGuiIO& io = ImGui::GetIO();
  const auto key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
  if (!down) io.AddKeyEvent(key, false);
  for (const ImGuiKey mod : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt}) {
    if ((chord & mod) != 0) io.AddKeyEvent(mod, down);
  }
  if (down) io.AddKeyEvent(key, true);
}

}  // namespace

std::expected<std::vector<ScriptAction>, Error> ParseScript(
    std::string_view text) {
  std::vector<ScriptAction> actions;
  std::uint32_t number = 0;
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view()
                                         : text.substr(end + 1);
    ++number;
    if (const std::size_t hash = line.find('#');
        hash != std::string_view::npos &&
        line.find('{') == std::string_view::npos) {
      line = line.substr(0, hash);
    }
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.remove_suffix(1);
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
      line.remove_prefix(1);
    }
    if (line.empty()) continue;
    ScriptAction action;
    action.line = number;
    const std::size_t space = line.find(' ');
    action.verb = std::string(line.substr(0, space));
    std::string_view rest = space == std::string_view::npos
                                ? std::string_view()
                                : line.substr(space + 1);
    const VerbSpec* spec = nullptr;
    for (const VerbSpec& candidate : kVerbs) {
      if (candidate.verb == action.verb) spec = &candidate;
    }
    if (spec == nullptr) {
      return std::unexpected(
          Error{ErrorCode::kParse,
                std::format("editor script: unknown action '{}'", action.verb),
                {},
                static_cast<std::int32_t>(number),
                0});
    }
    while (!rest.empty()) {
      while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
      if (rest.empty()) break;
      if (spec->rest_of_line && action.args.size() + 1 == spec->max_args) {
        action.args.emplace_back(rest);
        break;
      }
      const std::size_t next = rest.find(' ');
      action.args.emplace_back(rest.substr(0, next));
      rest = next == std::string_view::npos ? std::string_view()
                                            : rest.substr(next + 1);
    }
    if (action.args.size() < spec->min_args ||
        action.args.size() > spec->max_args) {
      return std::unexpected(Error{
          ErrorCode::kParse,
          std::format("editor script: '{}' takes {} to {} arguments, got {}",
                      action.verb, spec->min_args, spec->max_args,
                      action.args.size()),
          {},
          static_cast<std::int32_t>(number),
          0});
    }
    actions.push_back(std::move(action));
  }
  return actions;
}

// --- Documents ---

std::expected<void, Error> Editor::OpenDocument(App& app,
                                                const std::string& path,
                                                bool offer_recovery) {
  AssertMainThread();
  if (play_) {
    return Fail(ErrorCode::kInvalidArgument, "Stop before opening a document");
  }
  // A synchronous load on main: known long work to the watchdog (M12).
  const HeartbeatScope beat = app.KnownLongWork("open document");
  auto text = fs::ReadTextFile(path);
  if (!text) return std::unexpected(std::move(text.error()));
  DocumentDesc desc;
  desc.path = fs::Normalize(path);
  desc.name = StemOf(desc.path);
  desc.asset_root = fs::Join(app.Paths().content_root, game_.asset_dir);
  // The game's seed (GameSetup::seed through the module's StateEnv).
  desc.seed = 1;
  desc.tick_rate = kDefaultTickRate;
  std::unique_ptr<EditDocument> document;
  {
    const InlineOwnerScope owner(OnMainThread());
    auto opened =
        EditDocument::Open(app.GameInstance()->Registry(), game_, desc, *text);
    if (!opened) return std::unexpected(std::move(opened.error()));
    document = std::move(*opened);
  }
  // The old stack goes before the old document: its state shows it.
  if (document_states_) {
    const InlineOwnerScope owner(OnMainThread());
    document_states_->Clear();
    document_states_.reset();
  }
  document_ = std::move(document);
  autosave_.Reset(document_->Revision());
  last_save_revision_.reset();
  RebuildDocumentStack();
  SE_LOG_INFO("editor: opened {} ({} entities)", document_->Desc().path,
              document_->Get().EntityCount());
  if (offer_recovery) {
    recovery_offer_.clear();
    for (RecoveryFile& file : FindRecoveryFiles(app.Paths().pref)) {
      if (fs::Normalize(file.document) == document_->Desc().path) {
        SE_LOG_INFO("editor: recovery file {} (revision {}) for {}", file.path,
                    file.revision, file.document);
        recovery_offer_.push_back(std::move(file));
      }
    }
    if (!recovery_offer_.empty() && !recovery_answer_.empty()) {
      ResolveRecovery(app, recovery_answer_);
    }
  }
  return {};
}

void Editor::RebuildDocumentStack() {
  StateEnv env;
  env.registry = &document_->Get().Registry();
  env.world_kind = WorldKind::kEditDocument;
  // The state shows the document's World; it never steps.
  SetPendingDocumentWorld(document_->StateWorld());
  document_states_ = std::make_unique<GameStateStack>(document_factory_, env);
  const InlineOwnerScope owner(OnMainThread());
  document_states_->Request(StackOp::kPush, kDocumentState);
  document_states_->ApplyPending();
}

void Editor::ResolveRecovery(App& app, std::string_view answer) {
  if (recovery_offer_.empty()) return;
  RecoveryFile file = std::move(recovery_offer_.front());
  recovery_offer_.clear();
  if (answer == "accept" && document_ && !play_) {
    std::unique_ptr<EditDocument> recovered;
    {
      const InlineOwnerScope owner(OnMainThread());
      auto opened = EditDocument::Open(app.GameInstance()->Registry(), game_,
                                       document_->Desc(), file.scene);
      if (!opened) {
        SE_LOG_ERROR("editor: recovery {}: {}", file.path,
                     opened.error().message);
        return;
      }
      recovered = std::move(*opened);
    }
    {
      const InlineOwnerScope owner(OnMainThread());
      document_states_->Clear();
      document_states_.reset();
    }
    document_ = std::move(recovered);
    // Not saved: the recovered changes are unsaved changes.
    document_->MarkSaved(~std::uint64_t{0});
    autosave_.Reset(document_->Revision());
    last_save_revision_.reset();
    RebuildDocumentStack();
    SE_LOG_INFO("editor: recovered {} from {} (revision {})",
                document_->Desc().path, file.path, file.revision);
    return;
  }
  if (auto removed = fs::Remove(file.path); !removed) {
    SE_LOG_WARN("editor: {}: {}", file.path, removed.error().message);
  }
  SE_LOG_INFO("editor: recovery file {} discarded", file.path);
}

std::expected<void, Error> Editor::SaveDocument(App& app,
                                                const std::string& path,
                                                bool durable) {
  if (!document_) return Fail(ErrorCode::kInvalidArgument, "no document");
  document_->EndCoalesce();
  if (!path.empty()) document_->SetPath(fs::Normalize(path));
  if (document_->Desc().path.empty()) {
    return Fail(ErrorCode::kInvalidArgument, "the document has no file");
  }
  auto text = document_->Serialize();
  if (!text) return std::unexpected(std::move(text.error()));
  const Ticket ticket =
      SubmitWrite(app, document_->Desc().path, AsBytes(*text), durable);
  if (ticket == 0) {
    return Fail(ErrorCode::kIo, "the background lane took no save");
  }
  saves_.push_back(PendingSave{ticket, document_->Revision(),
                               document_->Desc().path, durable, false});
  save_error_.clear();
  SE_LOG_INFO("editor: saving {} (revision {}) through the lane",
              document_->Desc().path, document_->Revision());
  return {};
}

void Editor::Autosave(App& app, std::uint64_t now_ns) {
  if (!document_) return;
  // No input since: the document's activity is its edits too.
  const std::uint64_t quiet_since = std::max(last_input_ns_, last_change_ns_);
  if (document_->Revision() != seen_revision_) {
    seen_revision_ = document_->Revision();
    last_change_ns_ = now_ns;
    return;
  }
  if (!autosave_.Due(now_ns, quiet_since, document_->Revision(),
                     document_->Dirty())) {
    return;
  }
  auto text = document_->Serialize();
  if (!text) {
    SE_LOG_ERROR("editor: autosave: {}", text.error().message);
    autosave_.Submitted(document_->Revision());
    return;
  }
  RecoveryFile file;
  file.document = document_->Desc().path;
  file.name = document_->Desc().name;
  file.revision = document_->Revision();
  file.scene = std::move(*text);
  file.path = RecoveryPathFor(app.Paths().pref, file.document, file.name);
  const Ticket ticket =
      SubmitWrite(app, file.path, AsBytes(EncodeRecovery(file)),
                  /*durable=*/false);
  autosave_.Submitted(document_->Revision());
  if (ticket != 0) {
    saves_.push_back(
        PendingSave{ticket, document_->Revision(), file.path, false, true});
  }
  SE_LOG_INFO("editor: autosave of {} (revision {}) to {}", file.document,
              file.revision, file.path);
}

void Editor::WriteRecoveryFiles(QuitReason reason) {
  AssertMainThread();
  if (!document_ || !document_->Dirty() || app_ == nullptr) return;
  auto text = document_->Serialize();
  if (!text) {
    SE_LOG_ERROR("editor: recovery at {}: {}", QuitReasonName(reason),
                 text.error().message);
    return;
  }
  RecoveryFile file;
  file.document = document_->Desc().path;
  file.name = document_->Desc().name;
  file.revision = document_->Revision();
  file.scene = std::move(*text);
  file.path = RecoveryPathFor(app_->Paths().pref, file.document, file.name);
  if (auto written = WriteRecoveryNow(file); !written) {
    SE_LOG_ERROR("editor: recovery file {}: {}", file.path,
                 written.error().message);
    return;
  }
  SE_LOG_WARN("editor: {}: recovery file {} written (revision {})",
              QuitReasonName(reason), file.path, file.revision);
}

void Editor::OnLaneCompletion(const LaneCompletion& completion) {
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
      // A failed save keeps the document dirty; a quit waiting on it is
      // called off.
      save_error_ = std::format("{}: {}", save.path, error.message);
      if (quit_phase_ == QuitPhase::kSaving) quit_cancel_ = true;
    }
    SE_LOG_ERROR("editor: {} of {} failed: {}",
                 save.recovery ? "autosave" : "save", save.path, error.message);
    return;
  }
  if (!document_) return;
  const std::string recovery = RecoveryPathFor(
      app_->Paths().pref, document_->Desc().path, document_->Desc().name);
  const auto remove_recovery = [&recovery] {
    if (!fs::Exists(recovery)) return;
    if (auto removed = fs::Remove(recovery); !removed) {
      SE_LOG_WARN("editor: {}: {}", recovery, removed.error().message);
    }
  };
  if (save.recovery) {
    // An autosave that lands after a save of the same or a newer revision
    // completed holds older content than the file: it is no recovery.
    if (save.path == recovery && last_save_revision_.has_value() &&
        save.revision <= *last_save_revision_) {
      remove_recovery();
      SE_LOG_INFO(
          "editor: autosave of revision {} landed after the save of "
          "revision {}: recovery file removed",
          save.revision, *last_save_revision_);
    }
    return;
  }
  if (save.path != document_->Desc().path) return;
  document_->MarkSaved(save.revision);
  last_save_revision_ =
      std::max(last_save_revision_.value_or(0), save.revision);
  // A saved document needs no recovery file.
  remove_recovery();
  SE_LOG_INFO("editor: saved {} (revision {}){}", save.path, save.revision,
              document_->Dirty() ? "; changed since" : "");
}

void Editor::RunPendingOpen(App& app) {
  const auto open = [this, &app](const std::string& path) {
    AssertMainThread();
    if (auto opened = OpenDocument(app, path, /*offer_recovery=*/true);
        !opened) {
      SE_LOG_ERROR("editor: open {}: {}", path, opened.error().message);
    }
  };
  if (!open_after_save_.empty()) {
    const bool saving =
        std::any_of(saves_.begin(), saves_.end(),
                    [](const PendingSave& save) { return !save.recovery; });
    if (saving) return;
    std::string path = std::exchange(open_after_save_, {});
    if (document_ && document_->Dirty()) {
      SE_LOG_WARN("editor: open {} called off: '{}' is still unsaved{}", path,
                  document_->Desc().name,
                  save_error_.empty() ? std::string(" (changed since the save)")
                                      : " (" + save_error_ + ")");
      return;
    }
    open(path);
  }
  if (!pending_open_.has_value()) return;
  std::string path = std::move(*pending_open_);
  pending_open_.reset();
  if (document_ && document_->Dirty()) {
    // The open waits for the prompt (ADR-113): opening replaces the
    // document, and its unsaved changes with it.
    open_prompt_ = std::move(path);
    SE_LOG_INFO(
        "editor: '{}' has unsaved changes: opening {} waits for the "
        "prompt",
        document_->Desc().name, open_prompt_);
    if (!open_answer_.empty()) {
      ResolveOpenPrompt(app, std::exchange(open_answer_, {}));
    }
    return;
  }
  open(path);
}

void Editor::ResolveOpenPrompt(App& app, std::string_view answer) {
  if (open_prompt_.empty()) return;
  std::string path = std::exchange(open_prompt_, {});
  if (answer == "cancel") {
    SE_LOG_INFO("editor: open {} cancelled: '{}' keeps its changes", path,
                document_ ? document_->Desc().name : std::string());
    return;
  }
  if (answer == "save" && document_ && document_->Dirty()) {
    if (auto saved = SaveDocument(app, {}, /*durable=*/true); !saved) {
      save_error_ = saved.error().message;
      SE_LOG_ERROR("editor: open {} called off: the save failed: {}", path,
                   save_error_);
      return;
    }
    open_after_save_ = std::move(path);
    return;
  }
  SE_LOG_INFO("editor: open {}: the changes to '{}' are discarded", path,
              document_ ? document_->Desc().name : std::string());
  if (auto opened = OpenDocument(app, path, /*offer_recovery=*/true); !opened) {
    SE_LOG_ERROR("editor: open {}: {}", path, opened.error().message);
  }
}

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
    } else if (auto saved = SaveDocument(app, result.paths.front(),
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
  if (play_ || !document_) return;
  document_->EndCoalesce();
  auto text = document_->Serialize();
  if (!text) {
    SE_LOG_ERROR("play: the document did not serialize: {}",
                 text.error().message);
    return;
  }
  PlayStart start;
  start.kind = kind;
  start.bytes = AsBytes(*text);
  start.document_hash = document_->Hash();
  start.asset_root = document_->Desc().asset_root;
  start.seed = document_->Desc().seed;
  start.tick_rate = document_->Desc().tick_rate;
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
    session = PlaySession::Start(*app.GameInstance(), *app.GamePool(), game_,
                                 std::move(start), app.Assets());
  }
  if (!session) {
    SE_LOG_ERROR("play: {}", session.error().message);
    return;
  }
  play_ = std::move(*session);
  ++play_count_;
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
  const std::uint64_t now = document_ ? document_->Hash() : 0;
  SE_ASSERT_MSG(now == play_start_document_hash_,
                "Stop: the document's hash changed during Play");
  if (now != play_start_document_hash_) {
    SE_LOG_ERROR(
        "stop{} after tick {}: the document's hash changed during Play "
        "({:016x}, {:016x} at Play)",
        faulted ? " (from Play.Faulted)" : "", last, now,
        play_start_document_hash_);
  } else {
    SE_LOG_INFO("stop{} after tick {}: document hash {:016x} unchanged",
                faulted ? " (from Play.Faulted)" : "", last, now);
  }
}

// --- Quit ---

QuitPoll Editor::PollQuit(QuitReason /*reason*/) {
  // The QuitController polls its providers on main.
  AssertMainThread();
  // A file dialog is open: its callback first.
  if (dialogs_ && dialogs_->Pending() > 0) return QuitPoll::kPending;
  if (quit_cancel_) return QuitPoll::kCancel;
  // An open waiting for its prompt or its save does not survive the quit
  // (nothing acts on a dialog once the editor started quitting).
  if (!open_prompt_.empty() || !open_after_save_.empty()) {
    SE_LOG_INFO("editor: open {} dropped: the editor is quitting",
                open_prompt_.empty() ? open_after_save_ : open_prompt_);
    open_prompt_.clear();
    open_after_save_.clear();
  }
  switch (quit_phase_) {
    case QuitPhase::kNone:
      if (document_ && document_->Dirty()) {
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
  if (answer == "save" && document_ && document_->Dirty()) {
    if (auto saved = SaveDocument(app, {}, /*durable=*/true); !saved) {
      save_error_ = saved.error().message;
      quit_cancel_ = true;
      return;
    }
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

// --- The script ---

void Editor::ScriptFailure(const ScriptAction& action, std::string_view what) {
  ++script_failures_;
  SE_LOG_ERROR("editor script: line {}: FAILED {}: {}", action.line,
               action.verb, what);
}

void Editor::JoinScriptThread() noexcept {
  if (!script_thread_) return;
  script_thread_->thread.Join();
  script_thread_.reset();
}

void Editor::RunScript(App& app, std::uint64_t now_ns) {
  if (script_done_) return;
  if (script_next_ >= script_.size()) {
    script_done_ = true;
    if (script_failures_ == 0) {
      SE_LOG_INFO("editor script: passed ({} actions)", script_.size());
    } else {
      SE_LOG_ERROR("editor script: FAILED ({} of {} actions)", script_failures_,
                   script_.size());
      if (!app.Quit().Pending()) {
        quit_answer_ = "discard";
        app.RequestQuit();
      }
    }
    return;
  }
  // One action a frame: its commands apply in this frame's BuildFrame
  // before the next action looks at the document.
  const ScriptAction& action = script_[script_next_];
  if (wait_started_ns_ == 0) wait_started_ns_ = now_ns;
  if (RunAction(app, action)) {
    ++script_next_;
    wait_started_ns_ = 0;
    return;
  }
  if (now_ns - wait_started_ns_ > kScriptWaitLimitNs) {
    ScriptFailure(action, "timed out");
    ++script_next_;
    wait_started_ns_ = 0;
    wait_frames_ = 0;
    wait_until_tick_ = 0;
    steps_left_ = 0;
    stepping_ = false;
  }
}

bool Editor::RunAction(App& app, const ScriptAction& action) {
  const std::string& verb = action.verb;
  const std::vector<std::string>& args = action.args;
  auto guid_at = [&](std::size_t i) -> std::optional<Guid> {
    auto guid = ParseGuid(args[i]);
    if (!guid) {
      ScriptFailure(action, "bad Guid '" + args[i] + "'");
      return std::nullopt;
    }
    return *guid;
  };
  // The World edits apply to: the play World in Play, else the document.
  const World* world =
      play_ ? play_->TopWorld() : (document_ ? &document_->Get() : nullptr);
  auto submit = [&](std::unique_ptr<EditorCommand> command) {
    if (play_) {
      play_edits_->Submit(std::move(command), 0);
    } else {
      commands_->Submit(std::move(command));
    }
  };
  auto made = [&](auto result) -> bool {
    if (!result) {
      ScriptFailure(action, result.error().message);
      return true;
    }
    submit(std::move(*result));
    return true;
  };

  if (verb == "open") {
    if (auto opened = OpenDocument(
            app, fs::Join(app.Paths().content_root, args[0]), true);
        !opened) {
      ScriptFailure(action, opened.error().message);
    } else {
      initial_hash_ = document_->Hash();
    }
    return true;
  }
  if (verb == "select") {
    std::vector<Guid> guids;
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (auto guid = guid_at(i)) guids.push_back(*guid);
    }
    commands_->Select(guids);
    return true;
  }
  const bool edits_world = verb == "set" || verb == "add" || verb == "remove" ||
                           verb == "create" || verb == "create-copy" ||
                           verb == "destroy" || verb.starts_with("path-");
  if (world == nullptr && edits_world) {
    ScriptFailure(action, "no World to edit");
    return true;
  }
  if (verb == "set" || verb == "add" || verb == "remove") {
    const auto guid = guid_at(0);
    if (!guid) return true;
    if (verb == "set") {
      return made(SetComponentCommand::Make(*world, *guid, args[1], args[2]));
    }
    if (verb == "add") {
      submit(std::make_unique<AddComponentCommand>(*guid, args[1], args[2]));
      return true;
    }
    submit(std::make_unique<RemoveComponentCommand>(*guid, args[1]));
    return true;
  }
  if (verb == "create" || verb == "create-copy") {
    const auto guid = guid_at(0);
    if (!guid) return true;
    std::vector<ComponentJson> components;
    if (verb == "create-copy") {
      const auto from = guid_at(1);
      if (!from) return true;
      auto bundle = SerializeEntity(*world, *from);
      if (!bundle) {
        ScriptFailure(action, bundle.error().message);
        return true;
      }
      components = std::move(*bundle);
    } else {
      auto object = json::Parse(args[1]);
      if (!object || object->kind != json::Kind::kObject) {
        ScriptFailure(action, "create takes a JSON object of components");
        return true;
      }
      for (const json::Member& member : object->members) {
        components.push_back(
            ComponentJson{member.key, json::Write(member.value)});
      }
    }
    submit(std::make_unique<CreateEntityCommand>(*guid, std::move(components)));
    return true;
  }
  if (verb == "destroy") {
    const auto guid = guid_at(0);
    if (guid) submit(std::make_unique<DestroyEntityCommand>(*guid));
    return true;
  }
  if (verb.starts_with("path-")) {
    const auto guid = guid_at(0);
    const auto index = ParseU64(args[1]);
    if (!guid || !index) {
      if (guid) ScriptFailure(action, "bad index");
      return true;
    }
    const auto i = static_cast<std::uint32_t>(*index);
    if (verb == "path-delete") {
      return made(PathPointCommand::Delete(*world, *guid, i));
    }
    const auto x = ParseFloat(args[2]);
    const auto y = ParseFloat(args[3]);
    if (!x || !y) {
      ScriptFailure(action, "bad point");
      return true;
    }
    if (verb == "path-insert") {
      return made(PathPointCommand::Insert(*world, *guid, i, Vec2{*x, *y}));
    }
    return made(PathPointCommand::Move(*world, *guid, i, Vec2{*x, *y}));
  }
  if (verb == "coalesce-begin") {
    commands_->BeginCoalesce(args[0]);
    return true;
  }
  if (verb == "coalesce-end") {
    commands_->EndCoalesce();
    return true;
  }
  if (verb == "undo" || verb == "redo") {
    if (!document_ || play_) {
      ScriptFailure(action, "undo and redo are Edit-mode actions");
      return true;
    }
    const InlineOwnerScope owner(OnMainThread());
    auto done = verb == "undo" ? document_->Undo() : document_->Redo();
    if (!done) {
      ScriptFailure(action, done.error().message);
    } else if (!*done) {
      ScriptFailure(action, "nothing to " + verb);
    }
    return true;
  }
  if (verb == "save") {
    std::string path;
    if (!args.empty()) path = fs::Join(app.Paths().content_root, args[0]);
    if (auto saved = SaveDocument(app, path, /*durable=*/true); !saved) {
      ScriptFailure(action, saved.error().message);
    }
    return true;
  }
  if (verb == "play" || verb == "play-game") {
    request_ = verb == "play" ? Request::kPlay : Request::kPlayGame;
    return true;
  }
  if (verb == "pause" || verb == "resume") {
    if (!play_) {
      ScriptFailure(action, "not playing");
      return true;
    }
    request_ = verb == "pause" ? Request::kPause : Request::kResume;
    return true;
  }
  if (verb == "stop") {
    if (!play_) {
      ScriptFailure(action, "not playing");
      return true;
    }
    request_ = Request::kStop;
    return true;
  }
  if (verb == "step") {
    if (!stepping_) {
      const auto count =
          args.empty() ? std::optional<std::uint64_t>(1) : ParseU64(args[0]);
      if (!count || *count == 0) {
        ScriptFailure(action, "bad step count");
        return true;
      }
      if (!play_ || play_->State() != PlayState::kPaused) {
        ScriptFailure(action, "step needs Play.Paused");
        return true;
      }
      steps_left_ = static_cast<int>(*count);
      stepping_ = true;
    }
    if (!play_ || play_->Faulted()) {
      steps_left_ = 0;
      stepping_ = false;
      ScriptFailure(action, "the session stopped stepping");
      return true;
    }
    // A step runs in the frame after its request; wait for it to finish.
    if (play_->State() != PlayState::kPaused || request_ == Request::kStep) {
      return false;
    }
    if (steps_left_ == 0) {
      stepping_ = false;
      return true;
    }
    --steps_left_;
    request_ = Request::kStep;
    return false;
  }
  if (verb == "inject-throw") {
    if (!play_ || play_->Faulted()) {
      ScriptFailure(action, "inject-throw needs a play session");
      return true;
    }
    if (args[0] == "tick") {
      play_->InjectTickThrow();
    } else if (args[0] == "render") {
      play_->InjectRenderThrow();
    } else {
      ScriptFailure(action, "inject-throw takes tick or render");
    }
    return true;
  }
  if (verb == "wait-ticks") {
    if (!play_) {
      ScriptFailure(action, "not playing");
      return true;
    }
    if (wait_until_tick_ == 0) {
      const auto count = ParseU64(args[0]);
      if (!count) {
        ScriptFailure(action, "bad tick count");
        return true;
      }
      wait_until_tick_ = play_->LastTick() + *count;
    }
    if (play_->Faulted() || play_->LastTick() >= wait_until_tick_) {
      wait_until_tick_ = 0;
      return true;
    }
    return false;
  }
  if (verb == "wait-frames") {
    if (wait_frames_ == 0) {
      const auto count = ParseU64(args[0]);
      if (!count) {
        ScriptFailure(action, "bad frame count");
        return true;
      }
      wait_frames_ = *count + 1;
    }
    if (--wait_frames_ == 0) return true;
    return false;
  }
  if (verb == "dialog-open") {
    const DialogKind kind =
        args[0] == "save" ? DialogKind::kSave : DialogKind::kOpen;
    script_dialogs_.push_back(dialogs_->Open(kind).userdata);
    return true;
  }
  if (verb == "dialog-callback") {
    if (script_thread_) {
      // The thread's callback is still on its way.
      if (!script_thread_->done.load(std::memory_order_acquire)) return false;
      JoinScriptThread();
      return true;
    }
    if (script_dialogs_.empty()) {
      script_dialogs_.push_back(dialogs_->Open(DialogKind::kOpen).userdata);
    }
    void* userdata = script_dialogs_.front();
    script_dialogs_.pop_front();
    auto call = std::make_unique<ScriptThread>();
    call->userdata = userdata;
    if (args[0] != "cancel") {
      std::string_view rest = args[0];
      while (!rest.empty()) {
        const std::size_t semicolon = rest.find(';');
        const std::string path(rest.substr(0, semicolon));
        call->paths.push_back(fs::Join(app.Paths().content_root, path));
        rest = semicolon == std::string_view::npos ? std::string_view()
                                                   : rest.substr(semicolon + 1);
      }
    }
    for (const std::string& path : call->paths) {
      call->list.push_back(path.c_str());
    }
    call->list.push_back(nullptr);
    const bool from_thread = args.size() > 1 && args[1] == "from-thread";
    if (!from_thread) {
      DialogMailbox::Callback(call->userdata, call->list.data(), 0);
      return true;
    }
    ScriptThread* raw = call.get();
    ThreadSpec spec;
    spec.role = ThreadRole::kLibrary;
    spec.name = "dialog-test";
    auto started = StartThread(spec, [raw] {
      DialogMailbox::Callback(raw->userdata, raw->list.data(), 0);
      raw->done.store(true, std::memory_order_release);
    });
    if (!started) {
      ScriptFailure(action, started.error().message);
      DialogMailbox::Callback(call->userdata, call->list.data(), 0);
      return true;
    }
    call->thread = std::move(*started);
    script_thread_ = std::move(call);
    return false;
  }
  if (verb == "quit") {
    // The script goes on while the quit drains (dialogs, prompts, saves);
    // the process ends when the quit is done.
    quit_answer_ = args.empty() ? std::string("save") : args[0];
    SE_LOG_INFO("editor script: line {}: quit ({}), {} failures so far",
                action.line, quit_answer_, script_failures_);
    app.RequestQuit();
    return true;
  }
  if (verb == "terminate") {
    // SDL_EVENT_TERMINATING, as the OS sends it: the recovery hook writes
    // the dirty documents synchronously, then the app exits at once.
    SDL_Event event{};
    event.type = SDL_EVENT_TERMINATING;
    if (!SDL_PushEvent(&event)) ScriptFailure(action, SDL_GetError());
    return true;
  }
  if (verb == "key") {
    if (script_key_down_) {
      // The frame after the press: the UI saw it; release.
      FeedKeyChord(script_chord_, /*down=*/false);
      script_key_down_ = false;
      return true;
    }
    script_chord_ = ParseKeyChord(args[0]);
    if (script_chord_ == 0) {
      ScriptFailure(action, "unknown key '" + args[0] + "'");
      return true;
    }
    FeedKeyChord(script_chord_, /*down=*/true);
    script_key_down_ = true;
    return false;
  }
  if (verb == "push-key") {
    // An SDL key event as the OS would deliver it (SDL_PushEvent; never OS
    // input): the App's event handling, the editor's routing and the
    // game's InputService all see it.
    const std::uint32_t keycode = KeycodeFromName(args[0]);
    const bool down = args.size() < 2 || args[1] != "up";
    if (keycode == 0 || (args.size() > 1 && args[1] != "up")) {
      ScriptFailure(action, "push-key takes a key name and an optional up");
      return true;
    }
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.timestamp = SDL_GetTicksNS();
    event.key.windowID = app.GetWindow() != nullptr ? app.GetWindow()->Id() : 0;
    event.key.key = keycode;
    event.key.scancode = SDL_GetScancodeFromKey(keycode, nullptr);
    event.key.down = down;
    if (!SDL_PushEvent(&event)) {
      ScriptFailure(action, SDL_GetError());
    } else {
      SE_LOG_INFO("editor script: line {}: pushed key '{}' {}", action.line,
                  args[0], down ? "down" : "up");
    }
    return true;
  }
  if (verb == "answer-open") {
    if (args[0] != "save" && args[0] != "discard" && args[0] != "cancel") {
      ScriptFailure(action, "answer-open takes save, discard or cancel");
      return true;
    }
    if (open_prompt_.empty()) {
      // The answer for the next prompt.
      open_answer_ = args[0];
    } else {
      ResolveOpenPrompt(app, args[0]);
    }
    return true;
  }
  if (verb == "expect-open-prompt") {
    const bool want = YesNo(action, true);
    const bool have = !open_prompt_.empty();
    if (want != have) {
      ScriptFailure(action, have ? "an open waits for the prompt"
                                 : "no open waits for the prompt");
    }
    return true;
  }
  if (verb == "wait-autosave") {
    // The autosave of the document's revision was submitted.
    return !document_ || !document_->Dirty() ||
           autosave_.SubmittedRevision() == document_->Revision();
  }
  if (verb == "wait-saves") {
    // Every save and autosave in the lane completed.
    return saves_.empty();
  }
  if (verb == "expect-game-keyboard") {
    // The keyboard only: the real cursor may rest over the window, which
    // gives the game the mouse.
    const bool want = YesNo(action, true);
    const bool have =
        game_input_focus_ && (imgui_renderer_ == nullptr || game_view_focused_);
    if (have != want) {
      ScriptFailure(action,
                    std::format("the game {} the keyboard (view "
                                "focused {}, hovered {})",
                                have ? "has" : "lacks", game_view_focused_,
                                game_view_hovered_));
    }
    return true;
  }
  if (verb == "wait-ms") {
    const auto ms = ParseU64(args[0]);
    if (!ms) {
      ScriptFailure(action, "bad milliseconds");
      return true;
    }
    if (wait_until_ns_ == 0) {
      wait_until_ns_ = SDL_GetTicksNS() + *ms * 1'000'000;
    }
    if (SDL_GetTicksNS() < wait_until_ns_) return false;
    wait_until_ns_ = 0;
    return true;
  }
  if (verb == "expect-entities") {
    const auto want = ParseU64(args[1]);
    const World* counted = args[0] == "play"
                               ? (play_ ? play_->TopWorld() : nullptr)
                               : (document_ ? &document_->Get() : nullptr);
    if (!want || counted == nullptr) {
      ScriptFailure(action, "no such World, or a bad count");
    } else if (counted->EntityCount() != *want) {
      ScriptFailure(action, std::format("{} entities, expected {}",
                                        counted->EntityCount(), *want));
    }
    return true;
  }
  if (verb == "expect-recovery") {
    const bool want = YesNo(action, true);
    const bool have = document_ && fs::Exists(RecoveryPathFor(
                                       app.Paths().pref, document_->Desc().path,
                                       document_->Desc().name));
    if (want != have) {
      ScriptFailure(action,
                    have ? "a recovery file exists" : "no recovery file");
    }
    return true;
  }
  if (verb == "expect-quit-pending") {
    const bool want = YesNo(action, true);
    if (app.Quit().Pending() != want) {
      ScriptFailure(action, want ? "no quit pending" : "a quit is pending");
    }
    return true;
  }
  if (verb == "recovery") {
    recovery_answer_ = args[0];
    if (!recovery_offer_.empty()) ResolveRecovery(app, recovery_answer_);
    return true;
  }
  if (verb == "expect-hash") {
    std::uint64_t have = 0;
    if (args[0] == "document") {
      have = document_ ? document_->Hash() : 0;
    } else if (args[0] == "play") {
      if (!play_) {
        ScriptFailure(action, "not playing");
        return true;
      }
      have = play_->Hash();
    } else {
      ScriptFailure(action, "expect-hash takes document or play");
      return true;
    }
    std::optional<std::uint64_t> want;
    if (args[1] == "initial") {
      want = initial_hash_;
    } else if (args[1] == "play-start") {
      want = play_start_document_hash_;
    } else {
      want = ParseU64(args[1], 16);
    }
    if (!want) {
      ScriptFailure(action, "bad hash '" + args[1] + "'");
    } else if (*want != have) {
      ScriptFailure(action, std::format("{} hash {:016x}, expected {:016x}",
                                        args[0], have, *want));
    } else {
      SE_LOG_INFO("editor script: line {}: {} hash {:016x} as expected",
                  action.line, args[0], have);
    }
    return true;
  }
  if (verb == "expect-faulted") {
    const bool want = YesNo(action, true);
    const bool have = play_ && play_->Faulted();
    if (want != have) {
      ScriptFailure(action, have ? "faulted" : "not faulted");
    } else if (have) {
      const PlayFault& fault = play_->FaultInfo();
      SE_LOG_INFO("editor script: line {}: faulted ({} at tick {}: {})",
                  action.line, fault.where, fault.tick, fault.what);
    }
    return true;
  }
  if (verb == "expect-dirty") {
    const bool want = YesNo(action, true);
    const bool have = document_ && document_->Dirty();
    if (want != have) ScriptFailure(action, have ? "dirty" : "not dirty");
    return true;
  }
  if (verb == "show-panel") {
    bool known = false;
    for (const auto& panel : panels_) known = known || panel->Name() == args[0];
    if (!known) {
      ScriptFailure(action, "no panel '" + args[0] + "'");
      return true;
    }
    // Taken by the next UI frames (DrawUi), one window a frame.
    focus_queue_.push_back(args[0]);
    return true;
  }
  if (verb == "expect-panel") {
    if (!PanelDrewLastFrame(args[0])) {
      ScriptFailure(action,
                    std::format("panel '{}' did not draw in the last frame "
                                "({} draws so far)",
                                args[0], PanelDraws(args[0])));
    }
    return true;
  }
  if (verb == "expect-viewport") {
    // "window": exactly when the editor draws to a window (a headless
    // editor has no renderer and no viewports).
    const bool window = !args.empty() && args[0] == "window";
    const bool want = window ? app.Renderer() != nullptr : YesNo(action, true);
    const bool have = viewport_drawn_;
    if (want != have) {
      ScriptFailure(action,
                    have ? "a viewport was drawn" : "no viewport was drawn");
    }
    return true;
  }
  if (verb == "expect-fork-sites") {
    const auto count = ParseU64(args[0]);
    if (!count) {
      ScriptFailure(action, "bad count");
      return true;
    }
    std::size_t enabled = 0;
    for (const ForkSiteStats& site : fork_sites_) {
      if (site.enabled) ++enabled;
    }
    if (fork_sites_.size() < *count || enabled != 0 ||
        stats_.fork_workers != 0) {
      ScriptFailure(action, std::format("{} fork sites, {} enabled, {} workers",
                                        fork_sites_.size(), enabled,
                                        stats_.fork_workers));
    }
    return true;
  }
  if (verb == "expect-dump-stream") {
    const bool want = YesNo(action, true);
    const bool have = PlayDumpAttached();
    if (want != have) {
      ScriptFailure(action, have ? "the Play-start stream is registered"
                                 : "no Play-start stream is registered");
    }
    return true;
  }
  if (verb == "expect-state") {
    std::string have(EditorModeName(Mode()));
    for (char& ch : have) {
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (have != args[0]) ScriptFailure(action, "state is " + have);
    return true;
  }
  if (verb == "throttle-disk" || verb == "disk-busy") {
    const auto ms = ParseU64(args[0]);
    if (!ms) {
      ScriptFailure(action, "bad milliseconds");
      return true;
    }
    fs::FsFaults faults;
    if (verb == "throttle-disk") {
      faults.write_step_delay_ms = static_cast<std::uint32_t>(*ms);
    } else {
      faults.rename_busy_ms = static_cast<std::uint32_t>(*ms);
    }
    if (!fs::kFsFaultHooks) {
      ScriptFailure(action, "this build has no se::fs fault hooks");
      return true;
    }
    fs::SetFsFaults(faults);
    SE_LOG_INFO("editor script: {} {} ms", verb, *ms);
    return true;
  }
  if (verb == "dump-hash") {
    const std::string path = fs::Join(app.Paths().content_root, args[0]);
    const std::string text =
        std::format("document {:016x}\nplay {:016x}\ntick {}\n",
                    document_ ? document_->Hash() : 0,
                    play_ ? play_->Hash() : 0, play_ ? play_->LastTick() : 0);
    if (auto written = fs::WriteFile(path, AsBytes(text)); !written) {
      ScriptFailure(action, written.error().message);
    }
    return true;
  }
  ScriptFailure(action, "not handled");
  return true;
}

}  // namespace se::editor
