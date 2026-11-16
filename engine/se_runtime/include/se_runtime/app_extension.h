// Copyright 2026 The simple-engine-2 Authors.
//
// AppExtension: what the editor (M10a, se_editor) plugs into the App
// (docs/milestone-specs.md M10a; ADR-097). The App keeps its boot, its
// lifecycle, the pacer and stepper, the background lane, the renderer and
// the shutdown order; an extension owns the frame's session (what ticks
// and what is drawn) and adds its UI. Main thread only; every hook runs
// inside an SDL callback, so a throw from one takes the callbacks' fault
// path unless the extension contains it itself.
//
// With GameSetup::extension set, each iterate runs:
//   OnEvent       every SDL event before the App's own handling; true
//                 when the extension took the input (ImGui wants the mouse
//                 or keyboard): the game's InputService does not see it.
//                 Window, quit and lifecycle events are the App's anyway.
//   BeginFrame    after the inbox, the lane's completions and the quit
//                 poll, before the lifecycle update: the frame-start work
//                 (commands that release GPU resources, Stop's destroy,
//                 dialog results) and this frame's session.
//   TickFrame     the frame's ticks of the session, instead of the game's
//                 GameStateStack, with the App's plan (the FixedStepper,
//                 told the session's running and editor_play flags), the
//                 game's InputService and AudioBridge.
//   BuildFrame    after the ticks, before the render: the UI and the
//                 frame's render request (layers, viewports, overlay).
//   OnRenderFault a throw in the render build phase (a RenderList build);
//                 true when the extension handled it (Play.Faulted).
//   EndFrame      after the pacer's wait.
// The extension's BuildFrame runs every iterate, rendered or not (a
// headless App has no renderer: the extension still runs its UI frame).
// Shutdown: ShutdownWorlds at step 5 (before the game's Worlds), ShutdownUi
// at step 11 (after the asset scopes, before the renderer goes).

#ifndef SE_RUNTIME_APP_EXTENSION_H_
#define SE_RUNTIME_APP_EXTENSION_H_

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <expected>
#include <vector>

#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/time.h"
#include "se_core/types.h"
#include "se_render/frame_recorder.h"
#include "se_render/renderer_2d.h"
#include "se_sim/game_state.h"
#include "se_sim/sim_driver.h"

namespace se {

class App;
struct FrameReport;

// The frame's session as the App needs it.
struct ExtensionSession {
  // The stack whose states' assets load and hold (the AssetRuntime's
  // scopes follow it) and whose Worlds draw unless BuildFrame changes the
  // layers; null: nothing loads or draws.
  const GameStateStack* states = nullptr;
  // Ticks may run this frame (Play.Playing, Stepping, Play game).
  bool running = false;
  // Editor Play: Overloaded may run 0 ticks (FixedStepper's editor_play).
  bool editor_play = false;
  // The session's next tick (the title and the logs).
  Tick next_tick = 0;
  // Idle (waitevent after the lifecycle's quiet period) is allowed.
  bool idle_allowed = false;
  // The stepper's accumulator restarts (Pause and Resume: no catch-up).
  bool reset_accumulator = false;
};

// What the extension's ticks did.
struct ExtensionTicks {
  int ran = 0;
  Tick next_tick = 0;
};

// The frame's render request: the App fills the layers from the session's
// stack (as it does for a game), the extension may change everything.
struct ExtensionFrame {
  std::vector<RenderLayer>* layers = nullptr;
  std::vector<ViewportDraw>* viewports = nullptr;
  SwapchainOverlay* overlay = nullptr;
  ClearColor clear;
  // Where the World is drawn when it is not in the composite (the editor's
  // scene viewport): the --readback hooks read this target instead. Null:
  // the composite.
  const ViewportTarget* readback = nullptr;
};

class AppExtension {
 public:
  AppExtension() = default;
  AppExtension(const AppExtension&) = delete;
  AppExtension& operator=(const AppExtension&) = delete;
  virtual ~AppExtension() = default;

  // Boot step 10, after the game's runtime exists.
  [[nodiscard]] virtual std::expected<void, Error> Boot(App& app)
      SE_REQUIRES(g_main) = 0;
  virtual bool OnEvent(App& app, const SDL_Event& event)
      SE_REQUIRES(g_main) = 0;
  [[nodiscard]] virtual ExtensionSession BeginFrame(App& app,
                                                    std::uint64_t now_ns)
      SE_REQUIRES(g_main) = 0;
  [[nodiscard]] virtual ExtensionTicks TickFrame(App& app, const StepPlan& plan,
                                                 InputSource& input,
                                                 AudioSink& audio)
      SE_REQUIRES(g_main) = 0;
  virtual void BuildFrame(App& app, const FrameReport& report,
                          ExtensionFrame& frame) SE_REQUIRES(g_main) = 0;
  virtual bool OnRenderFault(App& app, const Error& error)
      SE_REQUIRES(g_main) = 0;
  virtual void EndFrame(App& app, const FrameReport& report)
      SE_REQUIRES(g_main) = 0;
  // Device loss (ADR-068): before the renderer releases its objects, and
  // after the new device and the renderer's objects exist.
  virtual void OnDeviceLost(App& app) SE_REQUIRES(g_main) = 0;
  virtual void OnDeviceRecreated(App& app) SE_REQUIRES(g_main) = 0;
  virtual void ShutdownWorlds(App& app) SE_REQUIRES(g_main) = 0;
  virtual void ShutdownUi(App& app) SE_REQUIRES(g_main) = 0;
  // The process should exit with a failure (a test script failed).
  [[nodiscard]] virtual bool ExitFailure() const noexcept { return false; }
};

}  // namespace se

#endif  // SE_RUNTIME_APP_EXTENSION_H_
