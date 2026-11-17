// Copyright 2026 The simple-engine-2 Authors.
//
// The Editor's nested helpers: the panels' EditorCommands, PlayEdits,
// EditorViewports and TextureIds, which queue into and read from the
// Editor (editor.h). Internal to se_editor.

#ifndef ENGINE_SE_EDITOR_SRC_EDITOR_INTERNAL_H_
#define ENGINE_SE_EDITOR_SRC_EDITOR_INTERNAL_H_

#include <SDL3/SDL_events.h>
#include <imgui.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/guid.h"
#include "se_core/handle.h"
#include "se_core/types.h"
#include "se_editor/commands.h"
#include "se_editor/editor.h"
#include "se_editor/panel.h"
#include "se_editor/texture_ids.h"
#include "se_os/thread.h"
#include "se_render/renderer_2d.h"

namespace se::editor {

class Editor::Commands final : public EditorCommands {
 public:
  explicit Commands(Editor* editor) : editor_(editor) {}
  void Submit(std::unique_ptr<EditorCommand> command) override;
  void BeginCoalesce(std::string_view label) override;
  void EndCoalesce() override;
  void Select(std::span<const Guid> guids) override;

 private:
  Editor* editor_;
};

class Editor::PlayEditsImpl final : public PlayEdits {
 public:
  explicit PlayEditsImpl(Editor* editor) : editor_(editor) {}
  [[nodiscard]] Tick NextTick() const noexcept override;
  void Submit(std::unique_ptr<EditorCommand> command,
              Tick apply_before) override;
  void Select(std::span<const Guid> guids) override;

 private:
  Editor* editor_;
};

class Editor::Viewports final : public EditorViewports {
 public:
  static constexpr std::uint32_t kInitialWidth = 640;
  static constexpr std::uint32_t kInitialHeight = 360;

  explicit Viewports(Editor* editor) : editor_(editor) {}
  [[nodiscard]] ViewportTarget* Viewport(std::string_view name) override;
  void ReportInput(bool focused, bool hovered) override;
  // The UI frame starts: no report yet.
  void StartReports() noexcept {
    focused_ = false;
    hovered_ = false;
  }
  [[nodiscard]] bool Focused() const noexcept { return focused_; }
  [[nodiscard]] bool Hovered() const noexcept { return hovered_; }
  // The panel that last reported (its window gets the focus at 'Play
  // game'); empty before any report.
  [[nodiscard]] const std::string& ReportingPanel() const noexcept {
    return reporting_panel_;
  }
  // The frame start: viewports nobody asked for last frame are destroyed.
  void BeginFrame();
  void DestroyAll();
  // The viewports asked for during this frame.
  [[nodiscard]] std::vector<ViewportTarget*> Asked() const;

 private:
  struct Entry {
    std::string name;
    ViewportTarget* target = nullptr;
    bool asked = false;
  };

  Editor* editor_;
  std::vector<Entry> entries_;
  bool focused_ = false;
  bool hovered_ = false;
  std::string reporting_panel_;
};

class Editor::Textures final : public TextureIds {
 public:
  explicit Textures(Editor* editor) : editor_(editor) {}
  [[nodiscard]] ImTextureID Id(TextureHandle texture) override;
  [[nodiscard]] ImVec2 Size(TextureHandle texture) override;

 private:
  Editor* editor_;
};

// dialog-callback from-thread: SDL's dialog thread, played by a named
// library thread (StartThread) that calls the mailbox's callback.
struct Editor::ScriptThread {
  Thread thread;
  std::atomic<bool> done{false};
  std::vector<std::string> paths;
  std::vector<const char*> list;
  void* userdata = nullptr;
};

// Keyboard, mouse, gamepad and text events (the autosave's quiet time,
// what ImGui and the game compete for).
[[nodiscard]] inline bool IsInputEvent(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] inline bool IsMouseEvent(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
      return true;
    default:
      return false;
  }
}

// A key, mouse button or gamepad button let go.
[[nodiscard]] inline bool IsReleaseEvent(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] inline std::vector<std::byte> AsBytes(std::string_view text) {
  const auto bytes = std::as_bytes(std::span<const char>(text));
  return std::vector<std::byte>(bytes.begin(), bytes.end());
}

}  // namespace se::editor

#endif  // ENGINE_SE_EDITOR_SRC_EDITOR_INTERNAL_H_
