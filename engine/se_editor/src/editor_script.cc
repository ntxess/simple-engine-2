// Copyright 2026 The simple-engine-2 Authors.
//
// The editor's test script (--editor-script; editor_script.h, ADR-099):
// parsing and running one action per frame.

#include "se_editor/editor_script.h"

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
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_editor/commands.h"
#include "se_editor/edit_document.h"
#include "se_editor/editor.h"
#include "se_editor/editor_stats.h"
#include "se_editor/play_session.h"
#include "se_editor/prefab_commands.h"
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
    {"disk-busy", 1, 2, false},
    {"wait-ms", 1, 1, false},
    {"expect-entities", 2, 2, false},
    {"expect-recovery", 0, 1, false},
    {"expect-quit-pending", 0, 1, false},
    {"terminate", 0, 0, false},
    {"key", 1, 1, false},
    {"push-key", 1, 2, false},
    {"answer-close", 1, 1, false},
    {"expect-close-prompt", 0, 1, false},
    // M11b: documents, prefabs, stat names, hot reload.
    {"activate", 1, 2, false},
    {"stress-press", 1, 1, false},
    {"close", 0, 1, false},
    {"save-all", 0, 0, false},
    {"expect-documents", 1, 1, false},
    {"expect-active", 1, 1, false},
    {"expect-content-hash", 2, 2, false},
    {"revert-to-prefab", 2, 2, false},
    {"expect-prefab", 2, 2, false},
    {"expect-override", 2, 3, false},
    {"expect-component", 2, 3, true},
    {"stat-name", 3, 3, false},
    {"expect-stat-name", 1, 2, false},
    {"write-file", 2, 2, true},
    {"wait-reload", 0, 0, false},
    {"answer-reload", 1, 1, false},
    {"expect-reload-prompt", 0, 1, false},
    {"expect-undo-label", 0, 1, true},
    {"wait-saves", 0, 0, false},
    {"wait-autosave", 0, 0, false},
    {"expect-game-keyboard", 0, 1, false},
    {"show-panel", 1, 1, true},
    {"expect-panel", 1, 1, true},
    {"expect-viewport", 0, 1, false},
    {"expect-fork-sites", 1, 1, false},
    {"expect-dump-stream", 0, 1, false},
    // M11/M13 integration: 'Play game' through the content gate, the
    // N-documents quit.
    {"expect-top-state", 1, 1, false},
    {"wait-top-state", 1, 1, false},
    {"expect-saved", 1, 1, false},
    {"write-document", 2, 2, false},
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

// The M11b verbs (documents, prefabs, stat names, hot reload); |handled|
// is false for any other verb. Returns what RunAction returns.
bool Editor::RunDocumentAction(App& app, const ScriptAction& action,
                               bool& handled) {
  const std::string& verb = action.verb;
  const std::vector<std::string>& args = action.args;
  handled = true;
  auto guid_at = [&](std::size_t i) -> std::optional<Guid> {
    if (args[i] == "root") {
      // A prefab document's one root entity.
      const EditDocument* prefab = Active();
      if (prefab == nullptr || prefab->Desc().kind != DocumentKind::kPrefab ||
          prefab->Get().EntityCount() != 1) {
        ScriptFailure(action, "'root' names a prefab document's root");
        return std::nullopt;
      }
      for (const auto [e, id] :
           prefab->Get().CReg().view<const IdComponent>().each()) {
        return id.guid;
      }
    }
    auto guid = ParseGuid(args[i]);
    if (!guid) {
      ScriptFailure(action, "bad Guid '" + args[i] + "'");
      return std::nullopt;
    }
    return *guid;
  };
  // The World edits apply to: the play World in Play, else the active
  // document.
  EditDocument* active = Active();
  DocumentEntry* entry = documents_.Active();
  const World* world = play_ ? play_->TopWorld()
                             : (active != nullptr ? &active->Get() : nullptr);
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

  if (verb == "activate") {
    // activate NAME|INDEX [refused]: "refused" expects the switch to be
    // refused (Play freezes the documents).
    const bool refused = args.size() > 1 && args[1] == "refused";
    DocumentEntry* found = documents_.FindByName(args[0]);
    if (found == nullptr) {
      ScriptFailure(action, "no document '" + args[0] + "'");
    } else if (ActivateDocument(found->id) == refused) {
      ScriptFailure(action, refused ? "the switch was not refused"
                                    : "the documents are frozen during Play");
    }
    return true;
  }
  if (verb == "stress-press") {
    const auto count = ParseU64(args[0]);
    if (!count || world == nullptr || play_ ||
        !options_.script_hooks.stress_press) {
      ScriptFailure(action, "stress-press N needs Edit mode and its hook");
      return true;
    }
    submit(options_.script_hooks.stress_press(
        *world, static_cast<std::uint32_t>(*count)));
    return true;
  }
  if (verb == "close") {
    if (entry == nullptr) {
      ScriptFailure(action, "no document to close");
      return true;
    }
    if (play_) {
      ScriptFailure(action, "the documents are frozen during Play");
      return true;
    }
    if (!args.empty()) {
      if (args[0] != "save" && args[0] != "discard" && args[0] != "cancel") {
        ScriptFailure(action, "close takes save, discard or cancel");
        return true;
      }
      close_answer_ = args[0];
    }
    RequestClose(app, entry->id);
    return true;
  }
  if (verb == "save-all") {
    if (!SaveAll(app)) ScriptFailure(action, save_error_);
    return true;
  }
  if (verb == "expect-documents") {
    const auto want = ParseU64(args[0]);
    if (!want || documents_.size() != *want) {
      ScriptFailure(action, std::format("{} documents open, expected {}",
                                        documents_.size(), args[0]));
    }
    return true;
  }
  if (verb == "expect-active") {
    if (active == nullptr || active->Desc().name != args[0]) {
      ScriptFailure(action, "the active document is '" +
                                (active ? active->Desc().name : std::string()) +
                                "'");
    }
    return true;
  }
  if (verb == "expect-content-hash") {
    if (args[0] != "document" || active == nullptr) {
      ScriptFailure(action, "expect-content-hash takes document");
      return true;
    }
    const std::uint64_t have = active->ContentHash();
    std::optional<std::uint64_t> want;
    if (args[1] == "initial") {
      want = entry->initial_content_hash;
    } else if (args[1] == "play-start") {
      want = play_start_content_hash_;
    } else {
      want = ParseU64(args[1], 16);
    }
    if (!want) {
      ScriptFailure(action, "bad hash '" + args[1] + "'");
    } else if (*want != have) {
      ScriptFailure(
          action,
          std::format("content hash {:016x}, expected {:016x}", have, *want));
    } else {
      SE_LOG_INFO("editor script: line {}: content hash {:016x} as expected",
                  action.line, have);
    }
    return true;
  }
  if (verb == "expect-prefab" || verb == "expect-override" ||
      verb == "revert-to-prefab" || verb == "expect-component") {
    const auto guid = guid_at(0);
    if (!guid) return true;
    if (world == nullptr || !world->Find(*guid)) {
      ScriptFailure(action, "no entity " + args[0]);
      return true;
    }
    if (verb == "expect-prefab") {
      const std::optional<std::string> prefab = PrefabOf(*world, *guid);
      const std::string have = prefab.value_or("none");
      if (have != args[1]) {
        ScriptFailure(action, "the entity's prefab is " + have);
      }
      return true;
    }
    if (verb == "expect-component") {
      // expect-component GUID COMPONENT [JSON|none]: held (with that JSON).
      const bool held = HoldsComponent(*world, *guid, args[1]);
      if (args.size() > 2 && args[2] == "none") {
        if (held) ScriptFailure(action, "the entity holds " + args[1]);
        return true;
      }
      if (!held) {
        ScriptFailure(action, "the entity lacks " + args[1]);
        return true;
      }
      if (args.size() > 2) {
        auto json = SerializeEntity(*world, *guid);
        std::string have;
        if (json) {
          for (const ComponentJson& c : *json) {
            if (c.component == args[1]) have = c.json;
          }
        }
        if (have != args[2]) {
          ScriptFailure(action, args[1] + " is " + have);
        }
      }
      return true;
    }
    if (!game_.prefab_baseline) {
      ScriptFailure(action, "the game has no prefab baseline");
      return true;
    }
    // Resolved as the panels resolve it: the document's recorded texts.
    auto baseline = game_.prefab_baseline(
        *world, *guid,
        active != nullptr ? active->Desc().asset_root : std::string(),
        active != nullptr && !play_ ? &active->PrefabTexts() : nullptr);
    if (!baseline) {
      ScriptFailure(action, baseline.error().message);
      return true;
    }
    if (verb == "expect-override") {
      const bool want = args.size() < 3 || args[2] == "yes";
      const bool have = DiffersFromBaseline(*world, *guid, args[1], *baseline);
      if (want != have) {
        ScriptFailure(action,
                      std::format("{} {} from the prefab", args[1],
                                  have ? "differs" : "does not differ"));
      }
      return true;
    }
    return made(MakeRevertToPrefab(*world, *guid, args[1], *baseline));
  }
  if (verb == "stat-name") {
    const auto guid = guid_at(0);
    const auto value = ParseFloat(args[2]);
    if (!guid) return true;
    if (!value) {
      ScriptFailure(action, "bad value '" + args[2] + "'");
      return true;
    }
    if (world == nullptr || !options_.script_hooks.stat_name_edit) {
      ScriptFailure(action, "no World, or no stat-name hook");
      return true;
    }
    return made(
        options_.script_hooks.stat_name_edit(*world, *guid, args[1], *value));
  }
  if (verb == "expect-stat-name") {
    const bool want = YesNo(
        ScriptAction{action.line, action.verb,
                     std::vector<std::string>(args.begin() + 1, args.end())},
        true);
    if (world == nullptr || !options_.script_hooks.has_stat_name) {
      ScriptFailure(action, "no World, or no stat-name hook");
      return true;
    }
    const bool have = options_.script_hooks.has_stat_name(*world, args[0]);
    if (want != have) {
      ScriptFailure(action, std::format("the stat name '{}' is {}", args[0],
                                        have ? "known" : "unknown"));
    }
    return true;
  }
  if (verb == "write-file") {
    // An external change for the hot-reload tests: a file under the
    // content root only (the tests run on a copy of it, never on a user's
    // files).
    if (fs::IsAbsolute(args[0]) || args[0].find("..") != std::string::npos) {
      ScriptFailure(action, "write-file takes a path under the content root");
      return true;
    }
    const std::string path = fs::Join(app.Paths().content_root, args[0]);
    std::string text = args[1];
    text += '\n';
    if (auto written = fs::WriteFileAtomic(path, AsBytes(text)); !written) {
      ScriptFailure(action, written.error().message);
    } else {
      SE_LOG_INFO("editor script: line {}: wrote {} ({} bytes)", action.line,
                  path, text.size());
    }
    return true;
  }
  if (verb == "wait-reload") {
    // Until a watched file's change was read and handled.
    if (!waiting_reload_) {
      waiting_reload_ = true;
      wait_reload_base_ = file_events_handled_;
    }
    if (file_events_handled_ > wait_reload_base_) {
      waiting_reload_ = false;
      return true;
    }
    return false;
  }
  if (verb == "answer-reload") {
    if (args[0] != "reload" && args[0] != "keep") {
      ScriptFailure(action, "answer-reload takes reload or keep");
      return true;
    }
    ResolveReloadPrompt(app, args[0]);
    return true;
  }
  if (verb == "expect-reload-prompt") {
    const bool want = YesNo(action, true);
    bool have = false;
    for (const auto& open : documents_.All()) {
      have = have || open->reload_prompt;
    }
    if (want != have) {
      ScriptFailure(action, have ? "a document asks to reload"
                                 : "no document asks to reload");
    }
    return true;
  }
  if (verb == "expect-undo-label") {
    const std::string have(active != nullptr ? active->UndoLabel() : "");
    const std::string want = args.empty() ? std::string() : args[0];
    if (have != want) ScriptFailure(action, "the undo label is '" + have + "'");
    return true;
  }
  handled = false;
  return true;
}

bool Editor::RunAction(App& app, const ScriptAction& action) {
  const std::string& verb = action.verb;
  const std::vector<std::string>& args = action.args;
  auto guid_at = [&](std::size_t i) -> std::optional<Guid> {
    if (args[i] == "root") {
      // A prefab document's one root entity.
      const EditDocument* prefab = Active();
      if (prefab == nullptr || prefab->Desc().kind != DocumentKind::kPrefab ||
          prefab->Get().EntityCount() != 1) {
        ScriptFailure(action, "'root' names a prefab document's root");
        return std::nullopt;
      }
      for (const auto [e, id] :
           prefab->Get().CReg().view<const IdComponent>().each()) {
        return id.guid;
      }
    }
    auto guid = ParseGuid(args[i]);
    if (!guid) {
      ScriptFailure(action, "bad Guid '" + args[i] + "'");
      return std::nullopt;
    }
    return *guid;
  };
  // The World edits apply to: the play World in Play, else the active
  // document.
  EditDocument* active = Active();
  DocumentEntry* entry = documents_.Active();
  const World* world = play_ ? play_->TopWorld()
                             : (active != nullptr ? &active->Get() : nullptr);
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
    // Adds a document, or activates the one open on the file.
    if (auto opened = OpenDocument(
            app, fs::Join(app.Paths().content_root, args[0]), true);
        !opened) {
      ScriptFailure(action, opened.error().message);
    }
    return true;
  }
  // M11b's documents, prefabs, stat names and hot reload.
  bool handled = false;
  const bool finished = RunDocumentAction(app, action, handled);
  if (handled) return finished;
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
                           verb == "destroy" || verb.starts_with("path-") ||
                           verb == "stat-name";
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
    // The Inspector's removal: the component and what derives from it, in
    // one undo step; refused for what the game keeps (ADR-119).
    return made(MakeRemoval(*world, *guid, args[1], game_));
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
    if (active == nullptr || play_) {
      ScriptFailure(action, "undo and redo are Edit-mode actions");
      return true;
    }
    edits_.EndScopeOf(documents_, documents_.ActiveId(), "an undo or redo");
    const InlineOwnerScope owner(OnMainThread());
    auto done = verb == "undo" ? active->Undo() : active->Redo();
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
    if (auto saved =
            SaveDocument(app, documents_.ActiveId(), path, /*durable=*/true);
        !saved) {
      ScriptFailure(action, saved.error().message);
    }
    return true;
  }
  if (verb == "play" || verb == "play-game") {
    if (!play_ && active != nullptr &&
        active->Desc().kind != DocumentKind::kScene) {
      ScriptFailure(action, "Play is disabled on a prefab document");
      return true;
    }
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
  if (verb == "answer-close") {
    if (args[0] != "save" && args[0] != "discard" && args[0] != "cancel") {
      ScriptFailure(action, "answer-close takes save, discard or cancel");
      return true;
    }
    if (close_prompt_ == kNoDocument) {
      // The answer for the next prompt.
      close_answer_ = args[0];
    } else {
      ResolveClosePrompt(app, args[0]);
    }
    return true;
  }
  if (verb == "expect-close-prompt") {
    const bool want = YesNo(action, true);
    const bool have = close_prompt_ != kNoDocument;
    if (want != have) {
      ScriptFailure(action, have ? "a close waits for the prompt"
                                 : "no close waits for the prompt");
    }
    return true;
  }
  if (verb == "wait-autosave") {
    // The autosave of the active document's revision was submitted.
    return active == nullptr || !active->Dirty() ||
           entry->autosave.SubmittedRevision() == active->Revision();
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
                               : (active != nullptr ? &active->Get() : nullptr);
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
    const bool have =
        active != nullptr &&
        fs::Exists(RecoveryPathFor(app.Paths().pref, active->Desc().path,
                                   active->Desc().name));
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
      have = active != nullptr ? active->Hash() : 0;
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
      want = entry != nullptr ? entry->initial_hash : 0;
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
    const bool have = active != nullptr && active->Dirty();
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
    // Each verb changes its own setting and keeps the other's.
    fs::FsFaults faults = fs::GetFsFaults();
    if (verb == "throttle-disk") {
      faults.write_step_delay_ms = static_cast<std::uint32_t>(*ms);
    } else {
      faults.rename_busy_ms = static_cast<std::uint32_t>(*ms);
      // disk-busy MS FILE: only renames onto FILE are refused.
      faults.rename_busy_suffix = args.size() > 1 ? args[1] : std::string();
    }
    if (!fs::kFsFaultHooks) {
      ScriptFailure(action, "this build has no se::fs fault hooks");
      return true;
    }
    fs::SetFsFaults(faults);
    SE_LOG_INFO("editor script: {} {} ms", verb, *ms);
    return true;
  }
  if (verb == "expect-top-state" || verb == "wait-top-state") {
    // The play session's top state (M13: 'Play game' moves between the
    // game's states); wait-top-state waits for it.
    const std::string_view top =
        play_ ? play_->States().TopId() : std::string_view();
    if (top == args[0]) return true;
    if (verb == "wait-top-state" && play_ && !play_->Faulted()) return false;
    ScriptFailure(action, std::format("the top state is '{}'", top));
    return true;
  }
  if (verb == "expect-saved" || verb == "write-document") {
    // expect-saved NAME: the document is clean and its file holds exactly
    // what it would save; write-document NAME FILE writes that text to
    // FILE under the content root (checked after the editor exited).
    DocumentEntry* found = documents_.FindByName(args[0]);
    if (found == nullptr) {
      ScriptFailure(action, "no document '" + args[0] + "'");
      return true;
    }
    const EditDocument& document = *found->document;
    auto text = document.SerializeFile();
    if (!text) {
      ScriptFailure(action, text.error().message);
      return true;
    }
    if (verb == "write-document") {
      if (fs::IsAbsolute(args[1]) || args[1].find("..") != std::string::npos) {
        ScriptFailure(action, "write-document takes a path under the root");
        return true;
      }
      const std::string path = fs::Join(app.Paths().content_root, args[1]);
      if (auto written = fs::WriteFile(path, AsBytes(*text)); !written) {
        ScriptFailure(action, written.error().message);
      }
      return true;
    }
    auto on_disk = fs::ReadTextFile(document.Desc().path);
    if (document.Dirty()) {
      ScriptFailure(action, "the document is dirty");
    } else if (!on_disk || *on_disk != *text) {
      ScriptFailure(action, std::format("{} does not hold the document's text",
                                        document.Desc().path));
    }
    return true;
  }
  if (verb == "dump-hash") {
    const std::string path = fs::Join(app.Paths().content_root, args[0]);
    const std::string text =
        std::format("document {:016x}\nplay {:016x}\ntick {}\n",
                    active != nullptr ? active->Hash() : 0,
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
