// Copyright 2026 The simple-engine-2 Authors.
//
// The rampage GameModule: components, systems and the "sandbox" gameplay
// state, which loads a scene (se-scene-v1) or a legacy scenario into one
// World. The rampage executable arrives in M13; until then the module runs
// through se_replay and the tests.

#ifndef GAME_RAMPAGE_MODULE_H_
#define GAME_RAMPAGE_MODULE_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "game_rampage/components.h"
#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"
#include "se_sim/game_module.h"

namespace se::rampage {

inline constexpr std::string_view kSandboxState = "sandbox";

// The actions, in the legacy LegacyAction order: bit i of InputFrame::held
// is action i, the mapping se_replay uses for .seinput files.
enum class Action : ActionId {
  kUp = 0,
  kLeft = 1,
  kDown = 2,
  kRight = 3,
  kJump = 4,
  kExit = 5,
  kEnter = 6,
  kBack = 7,
  kA1 = 8,
  kA2 = 9,
  kA3 = 10,
  kA4 = 11,
  kA5 = 12,
  kA6 = 13,
};

[[nodiscard]] const GameModule& RampageModule();

// What the sandbox state loads when the stack creates it.
struct SceneRequest {
  enum class Format : std::uint8_t {
    // An se-scene-v1 file; prefab references resolve against asset_root.
    kScene,
    // An se-legacy-scenario-v1 file; textures and config resolve against
    // legacy_root (the legacy tree runs from the repository root).
    kLegacyScenario,
  };
  Format format = Format::kScene;
  std::string path;
  std::string asset_root;
  std::string legacy_root;
  RulesVersion rules = RulesVersion::kV2;
};

// Sets the scene of the next sandbox state. Call it on the owner thread
// before the state is pushed (se_replay, tests; the runtime from its
// configuration in M13).
void SetSandboxScene(SceneRequest request);
[[nodiscard]] const SceneRequest& SandboxScene() noexcept;

// Loads |request| into an empty |world|: the scene settings into its ctx,
// then the entities. The sandbox state calls it in OnEnter; se_replay and
// the tests call it first to report errors.
std::expected<void, Error> LoadRequestedScene(World& world,
                                              const SceneRequest& request)
    SE_REQUIRES(g_sim_owner);

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_MODULE_H_
