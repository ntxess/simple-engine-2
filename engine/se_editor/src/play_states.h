// Copyright 2026 The simple-engine-2 Authors.
//
// The editor's GameStates (ADR-097): the play World of a Play session,
// registered under the module's start-state id (so the stack's hash is
// the one a headless replay of the same start bytes computes), and the
// document state, which holds a document World that never steps (the
// AssetRuntime gives it a scope like any state, so its textures load).
// A GameStateFactory creates states from a plain function, so the World a
// new state takes is handed over through a main-thread slot right before
// the push.

#ifndef ENGINE_SE_EDITOR_SRC_PLAY_STATES_H_
#define ENGINE_SE_EDITOR_SRC_PLAY_STATES_H_

#include <memory>
#include <string_view>

#include "se_core/world.h"
#include "se_sim/game_state.h"

namespace se::editor {

// The id of the document state.
inline constexpr std::string_view kDocumentState = "editor-document";

// Registers the play-World state under |id| (a string the factory may
// keep: the module's start_state).
void RegisterPlayWorldState(GameStateFactory& factory, std::string_view id);
// The World the next play-World state takes (main thread), and the flag
// that says whether it steps (false while Paused); the flag outlives the
// state.
void SetPendingPlayWorld(std::unique_ptr<World> world, const bool* running);

void RegisterDocumentState(GameStateFactory& factory);
// The World the next document state shows (not owned).
void SetPendingDocumentWorld(World* world);

}  // namespace se::editor

#endif  // ENGINE_SE_EDITOR_SRC_PLAY_STATES_H_
