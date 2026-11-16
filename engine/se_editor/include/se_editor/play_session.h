// Copyright 2026 The simple-engine-2 Authors.
//
// PlaySession: the editor's Play (docs/milestone-specs.md M10a, Play
// controls and Play.Faulted; docs/design/mt-design.md "Lifecycle", the Play
// states; ADR-097, ADR-099). Main thread only.
//
//   Play       the document serialized (the Play-start bytes) is loaded
//              with the game's loader into a fresh play World (kind kPlay:
//              mutable only through ticks, SessionLog entries and loads),
//              so every Play is a serializer round trip. The World is the
//              one state of the session's GameStateStack, registered under
//              the module's start-state id, so per-tick hashes equal a
//              headless replay's of the same start bytes (se_replay).
//   Play game  the module's own GameStateStack from its start state, the
//              start state pointed at the Play-start bytes written to a
//              file (EditorGame::set_play_game_scene).
// Both tick through their own SimDriver (from tick 1), SessionLog and
// ReplayRecorder (header and start bytes: the Play-start bytes), with the
// App's plan, the game's Schedule and fork pool, and the AssetRuntime as
// the TickGate (a state never steps before its assets are resident).
//
// States: Playing, Paused (no ticks; edits apply at once, logged against
// the next tick), Stepping (exactly one tick, then Paused), Faulted (an
// exception in a tick or in the play World's RenderList build: the World
// is frozen read-only, nothing ticks or edits it again; see Fault()).
// Pause, Resume and Step are logged in the SessionLog as markers with
// their tick. Edits (EditorCommand::ToSession) queue against a tick and
// apply at that tick's boundary; they never reach the document.
//
// Destroying the session clears its stack on main: the play World goes,
// and with it its voices (its AudioEvents' scope).

#ifndef SE_EDITOR_PLAY_SESSION_H_
#define SE_EDITOR_PLAY_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/breadcrumb.h"
#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/time.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/editor_game.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"

namespace se::editor {

enum class PlayState : std::uint8_t {
  kPlaying,
  kPaused,
  kStepping,
  kFaulted,
};
[[nodiscard]] std::string_view PlayStateName(PlayState state) noexcept;

enum class PlayKind : std::uint8_t { kPlay, kPlayGame };

struct PlayStart {
  PlayKind kind = PlayKind::kPlay;
  // The document as a scene file: the Play-start bytes.
  std::vector<std::byte> bytes;
  // The document's hash when Play started (Stop compares it).
  std::uint64_t document_hash = 0;
  std::string asset_root;
  // Play game: where the editor wrote |bytes| for the start state.
  std::string scene_path;
  std::uint64_t seed = 0;
  std::uint32_t tick_rate = kDefaultTickRate;
};

// Why a session faulted.
struct PlayFault {
  // "tick" or "render".
  std::string where;
  std::string what;
  // The tick that faulted (a tick fault) or the last tick run.
  Tick tick = 0;
  // The main thread's breadcrumb at the catch.
  BreadcrumbFields breadcrumb;
  // The system the breadcrumb names, when it names one.
  std::string system;
};

class PlaySession {
 public:
  // |instance| and |pool| outlive the session; |gate| (the AssetRuntime)
  // may be null.
  [[nodiscard]] static std::expected<std::unique_ptr<PlaySession>, Error> Start(
      ModuleInstance& instance, jobs::ForkJoinPool& pool,
      const EditorGame& game, PlayStart start, TickGate* gate)
      SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner);

  PlaySession(const PlaySession&) = delete;
  PlaySession& operator=(const PlaySession&) = delete;
  // Clears the stack on main (the states' OnExit, the Worlds and their
  // voices).
  ~PlaySession();

  [[nodiscard]] PlayState State() const noexcept { return state_; }
  [[nodiscard]] PlayKind Kind() const noexcept { return kind_; }
  [[nodiscard]] bool Faulted() const noexcept {
    return state_ == PlayState::kFaulted;
  }
  // Ticks may run this frame (the stepper's running flag): Playing.
  [[nodiscard]] bool Running() const noexcept {
    return state_ == PlayState::kPlaying;
  }

  // Each logged in the SessionLog with its tick; no-ops when faulted.
  void Pause() SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner);
  void Resume() SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner);
  // Paused: the next TickFrame() runs exactly one tick, then pauses again.
  void Step() SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner);

  // The frame's ticks: |plan|'s steps while Playing, one while Stepping,
  // none otherwise. A throw out of a tick enters Faulted (contained).
  // Returns the ticks run.
  int TickFrame(const StepPlan& plan, InputSource& input, AudioSink& audio,
                std::uint64_t now_ns) SE_REQUIRES(g_main)
      SE_EXCLUDES(g_sim_owner);
  // The results of the last TickFrame(), one per tick.
  [[nodiscard]] std::span<const TickResult> FrameResults() const noexcept {
    return driver_.FrameResults();
  }

  // An edit of the play World: |command|'s SessionLog entries against
  // |apply_before| (0, or a tick already run: the next tick). Paused, it
  // applies at once (Play; Play game queues it for its next tick).
  [[nodiscard]] std::expected<void, Error> Edit(const EditorCommand& command,
                                                Tick apply_before)
      SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner);

  // The play World's RenderList on the guarded path (contained like a
  // tick): false when the build threw. The first such throw enters
  // Faulted ("render"); one while already faulted only returns false (the
  // editor then discards the World).
  [[nodiscard]] bool BuildRenderList(const World& world, const Camera2D& camera,
                                     float alpha, RenderList& list)
      SE_REQUIRES(g_main);

  // A visual asset (|key|) hot-reloaded during the session: logged in the
  // SessionLog as an asset swap with its content hash (gameplay assets are
  // frozen until Stop).
  void LogAssetSwap(std::string_view key, std::uint64_t content_hash)
      SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner);

  // Enters Faulted (once): the World is frozen; the fault is kept for the
  // fault screen and the saved files.
  void Fault(PlayFault fault);
  [[nodiscard]] const PlayFault& FaultInfo() const noexcept { return fault_; }

  // Test hooks: the next tick throws (inside the tick, after its Worlds
  // stepped: the AudioSink's drain), or the next render build does
  // (TakeRenderThrow, read by the editor's guarded RenderList build).
  void InjectTickThrow() noexcept { inject_tick_throw_ = true; }
  void InjectRenderThrow() noexcept { inject_render_throw_ = true; }
  [[nodiscard]] bool TakeRenderThrow() noexcept {
    const bool take = inject_render_throw_;
    inject_render_throw_ = false;
    return take;
  }

  [[nodiscard]] Tick NextTick() const noexcept { return driver_.NextTick(); }
  // The last tick run (0 before the first).
  [[nodiscard]] Tick LastTick() const noexcept {
    return driver_.NextTick() - 1;
  }
  [[nodiscard]] const GameStateStack& States() const noexcept {
    return *states_;
  }
  // The top state's first World (what the panels show), or null.
  [[nodiscard]] const World* TopWorld() const;
  // The hash of the stack (the per-tick hash after the last tick).
  [[nodiscard]] std::uint64_t Hash() const;
  [[nodiscard]] const SessionLog& Log() const noexcept { return log_; }
  [[nodiscard]] const std::vector<std::byte>& StartBytes() const noexcept {
    return start_.bytes;
  }
  [[nodiscard]] std::uint64_t DocumentHash() const noexcept {
    return start_.document_hash;
  }
  // The replay so far (header, the Play-start bytes, every tick's frame
  // and hash, the SessionLog). Ends the recording: call once, at Stop or
  // at the fault.
  [[nodiscard]] std::vector<std::byte> FinishReplay();
  [[nodiscard]] bool ReplayFinished() const noexcept {
    return !recorder_.Active();
  }

 private:
  PlaySession(ModuleInstance& instance, jobs::ForkJoinPool& pool,
              PlayStart start);
  void Marker(SessionOp op) SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner);
  void SetRunning(bool running) noexcept;

  ModuleInstance* instance_;
  PlayStart start_;
  PlayKind kind_;
  PlayState state_ = PlayState::kPlaying;
  GameStateFactory factory_;
  std::unique_ptr<GameStateStack> states_;
  SimDriver driver_;
  SessionLog log_;
  ReplayRecorder recorder_;
  PlayFault fault_;
  // Whether the play World steps (its state's SimRunning); false while
  // Paused or Faulted, so SessionLog entries apply at once.
  bool play_running_ = true;
  bool inject_tick_throw_ = false;
  bool inject_render_throw_ = false;
};

}  // namespace se::editor

#endif  // SE_EDITOR_PLAY_SESSION_H_
