// Copyright 2026 The simple-engine-2 Authors.
//
// rampage's game flow (docs/milestone-specs.md, M13b): the Title, Gameplay,
// Pause and GameOver states and FlowSystem, the rules that move between
// them. Each state builds one World from a scene file read through
// StateEnv::content (the runtime's lane-loaded texts; tests and tools read
// synchronously) and puts a FlowContext with its role into the World's ctx.
// FlowSystem acts only in Worlds that carry one, so the "sandbox" state, the
// editor's Play, se_replay and every golden before M13 are unchanged.
//
// The rules (docs/rules/gameplay-rules.md, R14), one transition per tick at
// most, each applying at the next tick boundary:
//
//   Title     Enter: replace with "gameplay"; Exit: pop (the game ends)
//   Gameplay  no PlayerControl left alive (destroyed, or carrying a
//             DestroyRequest): replace with "game_over"; else Exit: push
//             "pause"
//   Pause     Exit: pop (Gameplay resumes); else Jump: pop, then replace
//             Gameplay with "title" (two commands, FIFO)
//   GameOver  Enter: replace with "gameplay" (a fresh World from the
//             scene); else Exit: replace with "title"
//
// An action counts when the tick's InputFrame has a press of it.

#ifndef GAME_RAMPAGE_FLOW_H_
#define GAME_RAMPAGE_FLOW_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "game_rampage/components.h"
#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/schedule.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"
#include "se_sim/content_source.h"
#include "se_sim/game_state.h"

namespace se::rampage {

// The states' ids.
inline constexpr std::string_view kTitleState = "title";
inline constexpr std::string_view kGameplayState = "gameplay";
inline constexpr std::string_view kPauseState = "pause";
inline constexpr std::string_view kGameOverState = "game_over";

// Their scenes, relative to the content root. Prefab references in a scene
// are relative to kContentAssetDir.
inline constexpr std::string_view kContentAssetDir = "assets";
inline constexpr std::string_view kTitleScene =
    "assets/scenes/title.scene.json";
inline constexpr std::string_view kDefaultGameplayScene =
    "assets/scenes/sandbox.scene.json";
inline constexpr std::string_view kPauseScene =
    "assets/scenes/pause.scene.json";
inline constexpr std::string_view kGameOverScene =
    "assets/scenes/game_over.scene.json";
// The key under which the editor's 'Play game' records its Gameplay scene
// (the Play-start bytes, read from a per-user absolute path) in the
// session's content bundle; se_replay points the Gameplay state at it when
// a bundle holds it, so a Play game replay or dump needs no --scene (review
// fix F4, ADR-129).
inline constexpr std::string_view kPlayGameSceneKey = "play-game.scene.json";

enum class FlowRole : std::uint8_t {
  kNone,
  kTitle,
  kGameplay,
  kPause,
  kGameOver
};

// In the ctx of a flow state's World: which rules FlowSystem applies there.
// Not part of the World's hash (the hash covers entities).
struct FlowContext {
  FlowRole role = FlowRole::kNone;
};

// The flow's rules: the last system of GameRules (after DeathRule, so a
// destroy requested this tick ends the Gameplay this tick).
struct FlowSystem {
  static constexpr std::string_view kName = "Flow";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner);
};

// What FlowSystem decides for a World of |role| in a tick with |input|;
// |player_alive| is whether a PlayerControl entity without a DestroyRequest
// is left. Empty: no transition. Pure, for tests.
[[nodiscard]] std::vector<StateCommand> FlowRules(FlowRole role,
                                                  const InputFrame& input,
                                                  bool player_alive);

// Whether |world| has a PlayerControl entity that is not being destroyed.
[[nodiscard]] bool PlayerAlive(const World& world);

// Where the flow states read their scenes.
struct FlowConfig {
  // The Gameplay scene, relative to the content root.
  std::string gameplay_scene = std::string(kDefaultGameplayScene);
  // Where files are read from when StateEnv::content is null (the editor's
  // Play before the integration; a tool without a content source).
  std::string content_root;
  RulesVersion rules = RulesVersion::kV2;
};

// Sets the flow's configuration for the states created from now on. Call it
// on the owner thread before the states are pushed (the rampage executable
// in its GameSetup::configure, tests).
void SetFlowConfig(FlowConfig config);
[[nodiscard]] const FlowConfig& GetFlowConfig();

// The scene the state |state_id| reads (relative to the content root):
// the flow states' scenes, the configured Gameplay scene; empty for any
// other state.
[[nodiscard]] std::string FlowSceneOf(std::string_view state_id);

// The text of |path| (relative to the content root) from |content|, or read
// from |content_root| when |content| is null.
[[nodiscard]] std::expected<std::string, Error> ReadContent(
    const ContentSource* content, std::string_view content_root,
    std::string_view path);

// Registers the four flow states.
void RegisterFlowStates(GameStateFactory& factory);

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_FLOW_H_
