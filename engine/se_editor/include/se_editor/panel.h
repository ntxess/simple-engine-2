// Copyright 2026 The simple-engine-2 Authors.
//
// The panel interface: the contract between the editor shell (M10a) and
// its panels (M10b) (docs/milestone-specs.md M10 "The panel interface";
// ADR-095).
//
// Panels are classes deriving from Panel. Every frame the editor draws each
// open panel on main inside the frame's ImGui frame, after the frame's
// ticks, as a window titled Name() in the editor's DockSpace, with a
// PanelContext valid for that Draw call only:
//   - world is the document World in Edit mode and the play World in Play
//     (Playing, Paused, Stepping, Faulted), so there is one inspector path
//     and no mirror; panels never mutate it (a document World's
//     MutationGate is closed outside command application, a play World's
//     outside ticks and SessionLog entries);
//   - changes go through EditorCommands in Edit mode (commands non-null),
//     and through PlayEdits in Play (play_edits non-null, SessionLog
//     entries against a tick); in Play.Faulted both are null: the World is
//     read-only;
//   - textures are shown through TextureIds, resolved every frame; a panel
//     keeps Handle<Texture>, never an ImTextureID or an SDL_GPUTexture*
//     (checks.gpu_texture_members);
//   - nothing in the context is kept past the call.
// Panels draw nothing but ImGui; the editor applies the frame's commands
// after ImGui::Render and before the RenderList build, so an edit shows in
// the same frame.

#ifndef SE_EDITOR_PANEL_H_
#define SE_EDITOR_PANEL_H_

#include <memory>
#include <span>
#include <string_view>

#include "se_core/guid.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/editor_stats.h"
#include "se_editor/log_view.h"
#include "se_editor/selection.h"
#include "se_editor/texture_ids.h"

namespace se {
class ViewportTarget;
}  // namespace se

namespace se::editor {

// Every Edit-mode mutation of the document, and the selection.
class EditorCommands {
 public:
  EditorCommands() = default;
  EditorCommands(const EditorCommands&) = delete;
  EditorCommands& operator=(const EditorCommands&) = delete;
  virtual ~EditorCommands() = default;

  // Queues |command|; it applies after this frame's ImGui::Render and goes
  // on the document's undo stack (into the open coalesce scope, if any).
  virtual void Submit(std::unique_ptr<EditorCommand> command) = 0;
  // A drag: every command submitted until EndCoalesce becomes one undo
  // step named |label|. Scopes do not nest; a second Begin ends the first.
  virtual void BeginCoalesce(std::string_view label) = 0;
  virtual void EndCoalesce() = 0;
  // Replaces the selection (not an undo step).
  virtual void Select(std::span<const Guid> guids) = 0;
};

// Edits of the play World while Playing, Paused or Stepping: SessionLog
// entries applied at a tick boundary (at once while paused), logged with
// their tick, replayed with the session, and discarded at Stop (the
// document is untouched).
class PlayEdits {
 public:
  PlayEdits() = default;
  PlayEdits(const PlayEdits&) = delete;
  PlayEdits& operator=(const PlayEdits&) = delete;
  virtual ~PlayEdits() = default;

  // The tick the play session runs next.
  [[nodiscard]] virtual Tick NextTick() const noexcept = 0;
  // Queues |command|'s change (EditorCommand::ToSession) against
  // |apply_before|; 0, or a tick already run, means the next tick. The
  // stress generator spreads spawns over ticks this way.
  virtual void Submit(std::unique_ptr<EditorCommand> command,
                      Tick apply_before) = 0;
  // Replaces the selection.
  virtual void Select(std::span<const Guid> guids) = 0;
};

// Offscreen viewports: the context's World drawn through a camera into a
// target the panel shows with ImGui::Image (M7's ViewportTarget).
class EditorViewports {
 public:
  EditorViewports() = default;
  EditorViewports(const EditorViewports&) = delete;
  EditorViewports& operator=(const EditorViewports&) = delete;
  virtual ~EditorViewports() = default;

  // The viewport named |name|, created on first use. This frame's World is
  // drawn into it (at alpha 1 while paused, edited or faulted) before the
  // ImGui pass; show TargetTexture() through TextureIds. RequestSize and
  // SetCamera take effect at the next frame start; a viewport not asked
  // for during a frame is destroyed at the next frame start. Null when
  // the editor has no renderer (headless).
  [[nodiscard]] virtual ViewportTarget* Viewport(std::string_view name) = 0;
};

// Read-only state a panel draws from, valid for one frame.
struct PanelContext {
  const World* world = nullptr;  // the document, or the play World
  bool playing = false;          // Play.Playing / Paused / Stepping
  bool faulted = false;          // Play.Faulted: read-only, no edits
  const Selection* selection = nullptr;
  EditorCommands* commands = nullptr;  // null while Playing or Faulted
  TextureIds* textures = nullptr;      // Handle<Texture> -> ImTextureID,
                                       // resolved every frame (never stored)
  const EditorStats* stats = nullptr;  // pacing, schedule, render, ForkStats
  LogView* log = nullptr;              // the main-owned formatted log ring
  double real_delta_sec = 0.0;

  // Additions to the spec's fields (ADR-095):
  bool paused = false;                   // Play.Paused or Play.Stepping
  PlayEdits* play_edits = nullptr;       // non-null while playing, not faulted
  EditorViewports* viewports = nullptr;  // null without a renderer
};

class Panel {
 public:
  Panel() = default;
  Panel(const Panel&) = delete;
  Panel& operator=(const Panel&) = delete;
  virtual ~Panel() = default;

  virtual std::string_view Name() const = 0;  // the window title
  virtual void Draw(const PanelContext& context) = 0;
};

}  // namespace se::editor

#endif  // SE_EDITOR_PANEL_H_
