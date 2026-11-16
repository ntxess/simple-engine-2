// Copyright 2026 The simple-engine-2 Authors.
//
// What every panel shares: where its edits go, component JSON read from the
// const World or made from an edited copy, and the widget probe the
// headless tests use to find a widget on screen.
//
// EditSink routes a panel's changes by the play state (se_editor/panel.h):
// Edit mode submits EditorCommands (undoable, coalesced while a drag
// lasts); Play (Playing, Paused, Stepping) queues the same commands in the
// play session's SessionLog through PlayEdits, against the next tick;
// Play.Faulted takes nothing (the World is read-only, CanEdit() is false).
// A command that cannot be made (an error from a Make function) is logged
// and dropped. Panels never touch the World themselves: they read it
// through CReg() and hand commands to the sink.

#ifndef SE_EDITOR_PANELS_PANEL_SUPPORT_H_
#define SE_EDITOR_PANELS_PANEL_SUPPORT_H_

#include <imgui.h>

#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/panel.h"

namespace se::editor {

class EditSink {
 public:
  explicit EditSink(const PanelContext& context) noexcept
      : context_(&context) {}

  // Edits are taken (Edit mode with commands, or Play with PlayEdits).
  [[nodiscard]] bool CanEdit() const noexcept;
  // Selection changes are taken.
  [[nodiscard]] bool CanSelect() const noexcept;
  // In Play: edits become SessionLog entries.
  [[nodiscard]] bool Playing() const noexcept;
  // The play session's next tick (0 in Edit mode).
  [[nodiscard]] Tick NextTick() const noexcept;

  // Hands |command| on; |apply_before| is the tick it applies before in
  // Play (0: the next tick), ignored in Edit mode. False when not taken.
  bool Submit(std::unique_ptr<EditorCommand> command, Tick apply_before = 0);
  // The same for a command still to be made: an error is logged and the
  // edit dropped.
  template <class C>
  bool Submit(std::expected<std::unique_ptr<C>, Error> command,
              Tick apply_before = 0) {
    if (!command) {
      ReportError(command.error());
      return false;
    }
    return Submit(std::unique_ptr<EditorCommand>(std::move(*command)),
                  apply_before);
  }
  // A drag: one undo step in Edit mode; nothing in Play (each frame's
  // change is its own SessionLog entry).
  void BeginCoalesce(std::string_view label);
  void EndCoalesce();
  bool Select(std::span<const Guid> guids);

  [[nodiscard]] const PanelContext& Context() const noexcept {
    return *context_;
  }

  static void ReportError(const Error& error);

 private:
  const PanelContext* context_;
};

// Begins the sink's coalesce scope when the last item was activated and
// ends it when it was deactivated (a drag or a text edit is one undo step).
void TrackCoalesce(EditSink& sink, std::string_view label);

// The component named |name| (registered), or null.
[[nodiscard]] const ComponentInfo* FindInfo(const World& world,
                                            std::string_view name) noexcept;
// Whether |entity| has the component of |info|.
[[nodiscard]] bool HasComponent(const World& world, entt::entity entity,
                                const ComponentInfo& info) noexcept;
// The component's JSON as the registry writes it.
[[nodiscard]] std::expected<std::string, Error> ReadComponentJson(
    const World& world, entt::entity entity, std::string_view name);

// The JSON the registry writes for |value| as the component |name| (its
// type must be T): the "after" of an edit made on a copy.
template <class T>
[[nodiscard]] std::expected<std::string, Error> JsonOfValue(
    const World& world, std::string_view name, const T& value) {
  const ComponentInfo* info = FindInfo(world, name);
  if (info == nullptr || info->write_json == nullptr) {
    return Fail(ErrorCode::kNotFound, "no such component");
  }
  entt::registry scratch;
  const entt::entity e = scratch.create();
  scratch.emplace<T>(e, value);
  return info->write_json(scratch, e);
}

// A set-component command from |before| to the JSON of |after|; nullopt
// when they are the same text (nothing to do).
template <class T>
[[nodiscard]] std::expected<std::unique_ptr<EditorCommand>, Error> SetFromValue(
    const World& world, Guid target, std::string_view name, std::string before,
    const T& after) {
  auto json = JsonOfValue(world, name, after);
  if (!json) return std::unexpected(std::move(json.error()));
  if (*json == before) return std::unique_ptr<EditorCommand>{};
  return std::unique_ptr<EditorCommand>(std::make_unique<SetComponentCommand>(
      target, std::string(name), std::move(before), std::move(*json)));
}

// Screen rectangles of named widgets, recorded while a panel draws (tests
// click on them with synthetic ImGui IO events). Panels record only when a
// probe is attached.
class WidgetProbe {
 public:
  void Record(std::string key, ImVec2 min, ImVec2 max);
  // The last item drawn (ImGui::GetItemRectMin/Max).
  void RecordLastItem(std::string key);
  [[nodiscard]] std::optional<Rect> Find(std::string_view key) const;
  [[nodiscard]] std::optional<ImVec2> Center(std::string_view key) const;
  void Clear() noexcept { rects_.clear(); }
  [[nodiscard]] std::size_t size() const noexcept { return rects_.size(); }

 private:
  std::vector<std::pair<std::string, Rect>> rects_;
};

// A probe the panel owns a pointer to (nullptr: not recording).
class Probed {
 public:
  void SetProbe(WidgetProbe* probe) noexcept { probe_ = probe; }

 protected:
  void ProbeItem(std::string_view key) const;
  void ProbeRect(std::string_view key, ImVec2 min, ImVec2 max) const;
  [[nodiscard]] WidgetProbe* Probe() const noexcept { return probe_; }

 private:
  WidgetProbe* probe_ = nullptr;
};

// Text being typed into an InputText, kept until the edit ends so a value
// derived from the World each frame does not overwrite it.
class TextEdits {
 public:
  // Draws an InputText for |current|; returns the committed text when an
  // edit ended this frame with a change (Enter or focus lost).
  std::optional<std::string> Input(const char* label,
                                   const std::string& current,
                                   float width = 0.0F);

 private:
  std::unordered_map<ImGuiID, std::string> buffers_;
};

// Formats a Guid's first 8 hex digits (the hierarchy and overlays).
[[nodiscard]] std::string ShortGuid(const Guid& guid);

// The entity's Name component, or empty.
[[nodiscard]] std::string_view EntityName(const World& world,
                                          entt::entity entity);

}  // namespace se::editor

#endif  // SE_EDITOR_PANELS_PANEL_SUPPORT_H_
