// Copyright 2026 The simple-engine-2 Authors.
//
// Headless ImGui for the panel tests: an ImGui context with se_imgui's
// HeadlessFrames (no window, no GPU), a frame loop that draws panels the
// way the editor does (each in the window titled Name()), synthetic input
// through ImGui's own IO calls (io.AddMousePosEvent and friends, never OS
// input), and fakes of the editor's interfaces: EditorCommands and
// PlayEdits that record what panels hand them, and TextureIds.
//
// RecordingCommands::ApplyPending applies the recorded commands to a World
// with its gate open, as the editor applies a frame's commands after
// ImGui::Render, so multi-frame interactions see their own edits.

#ifndef TESTS_EDITOR_PANELS_SUPPORT_PANEL_HARNESS_H_
#define TESTS_EDITOR_PANELS_SUPPORT_PANEL_HARNESS_H_

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/guid.h"
#include "se_core/handle.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/panel.h"
#include "se_editor/selection.h"
#include "se_editor/texture_ids.h"
#include "se_imgui/imgui_context.h"

namespace se::testing {

class RecordingCommands final : public editor::EditorCommands {
 public:
  void Submit(std::unique_ptr<editor::EditorCommand> command) override {
    events.push_back("submit:" + std::string(command->Label()));
    commands.push_back(std::move(command));
  }
  void BeginCoalesce(std::string_view label) override {
    events.push_back("begin:" + std::string(label));
  }
  void EndCoalesce() override { events.emplace_back("end"); }
  void Select(std::span<const Guid> guids) override {
    selections.emplace_back(guids.begin(), guids.end());
    selection.Set(guids);
  }

  // Applies the commands recorded since the last call to |world| (its gate
  // open, as the editor's command application opens it); false when one
  // failed.
  bool ApplyPending(World& world) SE_REQUIRES(g_sim_owner);
  void Clear() {
    commands.clear();
    events.clear();
    selections.clear();
    applied_ = 0;
  }

  std::vector<std::unique_ptr<editor::EditorCommand>> commands;
  std::vector<std::string> events;
  std::vector<std::vector<Guid>> selections;
  editor::Selection selection;

 private:
  std::size_t applied_ = 0;
};

class RecordingPlayEdits final : public editor::PlayEdits {
 public:
  [[nodiscard]] Tick NextTick() const noexcept override { return next_tick; }
  void Submit(std::unique_ptr<editor::EditorCommand> command,
              Tick apply_before) override {
    queued.emplace_back(apply_before, std::move(command));
  }
  void Select(std::span<const Guid> guids) override {
    selection.Set(guids);
    ++selects;
  }

  Tick next_tick = 1;
  std::vector<std::pair<Tick, std::unique_ptr<editor::EditorCommand>>> queued;
  editor::Selection selection;
  int selects = 0;
};

class FakeTextureIds final : public editor::TextureIds {
 public:
  [[nodiscard]] ImTextureID Id(TextureHandle /*texture*/) override {
    ++lookups;
    return static_cast<ImTextureID>(7);
  }
  [[nodiscard]] ImVec2 Size(TextureHandle /*texture*/) override {
    return {1.0F, 1.0F};
  }
  int lookups = 0;
};

inline constexpr float kDisplayWidth = 1280.0F;
inline constexpr float kDisplayHeight = 800.0F;

class PanelHarness {
 public:
  PanelHarness() SE_REQUIRES(g_main);
  ~PanelHarness();
  PanelHarness(const PanelHarness&) = delete;
  PanelHarness& operator=(const PanelHarness&) = delete;

  // One frame: every panel in its own window (the first fills the display;
  // others are stacked below it, off the first one's area), then Render.
  void Frame(std::span<editor::Panel* const> panels,
             const editor::PanelContext& context) SE_REQUIRES(g_main);
  void Frame(editor::Panel& panel, const editor::PanelContext& context)
      SE_REQUIRES(g_main) {
    editor::Panel* const one[] = {&panel};
    Frame(one, context);
  }

  // The first panel's window width from the next frame (a narrow docked
  // panel); the display's width by default.
  void SetFirstWidth(float width) noexcept { first_width_ = width; }

  // Input for the next frame (ImGui's IO event queue).
  void MoveMouse(ImVec2 pos);
  void Button(int button, bool down);
  void Wheel(float notches);
  void Key(ImGuiKey key, bool down);
  void Type(std::string_view text);

  // A click at |pos|: two frames moving the mouse there, a frame with the
  // press, a frame with the release.
  void Click(editor::Panel& panel, const editor::PanelContext& context,
             ImVec2 pos, int button = 0) SE_REQUIRES(g_main);

 private:
  float first_width_ = kDisplayWidth;
  std::unique_ptr<imgui::Context> context_;
  std::unique_ptr<imgui::HeadlessFrames> frames_;
};

}  // namespace se::testing

#endif  // TESTS_EDITOR_PANELS_SUPPORT_PANEL_HARNESS_H_
