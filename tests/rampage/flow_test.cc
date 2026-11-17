// Copyright 2026 The simple-engine-2 Authors.
//
// rampage's game flow (M13b): each rule of FlowSystem from synthetic
// InputFrames on a GameStateStack of the module with a CachedContentSource;
// one transition per tick, applied at the next tick boundary; GameOver on
// the player's destroy request; Pause's two-command return; the sandbox
// state's World has no FlowContext; the flow's scenes and the sandbox load
// the same through a content source as from files; and SceneDependencies.

#include "game_rampage/flow.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/content.h"
#include "game_rampage/module.h"
#include "game_rampage/scene.h"
#include "rampage/support/flow_session.h"
#include "rampage/support/rampage_session.h"
#include "se_core/core_components.h"
#include "se_core/input_frame.h"
#include "se_core/thread_role.h"
#include "se_core/time.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "support/test_roles.h"

namespace se::rampage {
namespace {

using Ids = std::vector<std::string>;

const World& Top(testing::FlowHarness& flow) {
  const std::vector<World*> worlds = flow.States().TopWorlds();
  REQUIRE(worlds.size() == 1);
  return *worlds.front();
}

InputFrame Pressing(std::initializer_list<Action> actions) {
  InputFrame frame;
  for (const Action action : actions) {
    const auto id = static_cast<ActionId>(action);
    frame.held |= 1ULL << id;
    frame.pressed[id] = 1;
  }
  return frame;
}

bool IsCommand(const StateCommand& command, StackOp op, std::string_view id) {
  return command.op == op && command.state_id == id;
}

TEST_CASE("flow.The rules from synthetic InputFrames", "[rampage][flow]") {
  const InputFrame none;
  const InputFrame enter = Pressing({Action::kEnter});
  const InputFrame exit = Pressing({Action::kExit});
  const InputFrame jump = Pressing({Action::kJump});

  // Nothing pressed, nothing happens (the player alive).
  for (const FlowRole role :
       {FlowRole::kNone, FlowRole::kTitle, FlowRole::kGameplay,
        FlowRole::kPause, FlowRole::kGameOver}) {
    CHECK(FlowRules(role, none, true).empty());
  }
  // A World without a role never transitions.
  CHECK(FlowRules(FlowRole::kNone, exit, false).empty());

  auto title = FlowRules(FlowRole::kTitle, enter, true);
  REQUIRE(title.size() == 1);
  CHECK(IsCommand(title[0], StackOp::kReplace, kGameplayState));
  title = FlowRules(FlowRole::kTitle, exit, true);
  REQUIRE(title.size() == 1);
  CHECK(IsCommand(title[0], StackOp::kPop, {}));
  CHECK(FlowRules(FlowRole::kTitle, jump, true).empty());

  auto gameplay = FlowRules(FlowRole::kGameplay, exit, true);
  REQUIRE(gameplay.size() == 1);
  CHECK(IsCommand(gameplay[0], StackOp::kPush, kPauseState));
  gameplay = FlowRules(FlowRole::kGameplay, none, false);
  REQUIRE(gameplay.size() == 1);
  CHECK(IsCommand(gameplay[0], StackOp::kReplace, kGameOverState));
  CHECK(FlowRules(FlowRole::kGameplay, enter, true).empty());
  CHECK(FlowRules(FlowRole::kGameplay, jump, true).empty());

  auto pause = FlowRules(FlowRole::kPause, exit, true);
  REQUIRE(pause.size() == 1);
  CHECK(IsCommand(pause[0], StackOp::kPop, {}));
  pause = FlowRules(FlowRole::kPause, jump, true);
  REQUIRE(pause.size() == 2);
  CHECK(IsCommand(pause[0], StackOp::kPop, {}));
  CHECK(IsCommand(pause[1], StackOp::kReplace, kTitleState));
  CHECK(FlowRules(FlowRole::kPause, enter, true).empty());

  auto over = FlowRules(FlowRole::kGameOver, enter, true);
  REQUIRE(over.size() == 1);
  CHECK(IsCommand(over[0], StackOp::kReplace, kGameplayState));
  over = FlowRules(FlowRole::kGameOver, exit, true);
  REQUIRE(over.size() == 1);
  CHECK(IsCommand(over[0], StackOp::kReplace, kTitleState));
}

TEST_CASE("flow.One transition per tick", "[rampage][flow]") {
  // Two keys in one tick: the first rule that matches wins.
  auto title = FlowRules(FlowRole::kTitle,
                         Pressing({Action::kEnter, Action::kExit}), true);
  REQUIRE(title.size() == 1);
  CHECK(IsCommand(title[0], StackOp::kReplace, kGameplayState));
  auto pause = FlowRules(FlowRole::kPause,
                         Pressing({Action::kExit, Action::kJump}), true);
  REQUIRE(pause.size() == 1);
  CHECK(IsCommand(pause[0], StackOp::kPop, {}));
  // The death wins over a pause asked for in the same tick.
  auto gameplay =
      FlowRules(FlowRole::kGameplay, Pressing({Action::kExit}), false);
  REQUIRE(gameplay.size() == 1);
  CHECK(IsCommand(gameplay[0], StackOp::kReplace, kGameOverState));

  // On the stack: a press applies at the next boundary, never in its own
  // tick, and the new state's World runs that next tick.
  const testing::MainThreadScope main_thread;
  testing::FlowHarness flow;
  flow.Start(kTitleState);
  TickResult tick = flow.Press(Action::kEnter);
  CHECK(tick.top_state == kTitleState);
  CHECK(flow.Ids() == Ids{"title"});
  REQUIRE(flow.States().PendingCount() == 0);
  tick = flow.Idle();
  CHECK(tick.top_state == kGameplayState);
  REQUIRE(tick.worlds_stepped == 1);
  CHECK(flow.Ids() == Ids{"gameplay"});
  // The Gameplay's World ran exactly one tick.
  REQUIRE(Top(flow).Clock().tick == 1);
  // Escape held over several ticks pauses once: only its press counts.
  InputFrame held;
  held.held = 1ULL << static_cast<ActionId>(Action::kExit);
  (void)flow.Press(Action::kExit);
  (void)flow.Tick(held);
  (void)flow.Tick(held);
  CHECK(flow.Ids() == Ids{"gameplay", "pause"});
  flow.ApplyPending();
  CHECK(flow.Ids() == Ids{"gameplay", "pause"});
}

TEST_CASE("flow.Title's Enter starts Gameplay and Exit ends the game",
          "[rampage][flow]") {
  const testing::MainThreadScope main_thread;
  testing::FlowHarness flow;
  flow.Start(kTitleState);
  REQUIRE(flow.Ids() == Ids{"title"});
  // The Title is the wallpaper, nothing else.
  const World& title = Top(flow);
  REQUIRE(title.EntityCount() == 1);
  REQUIRE(title.CReg().ctx().find<FlowContext>() != nullptr);
  CHECK(title.CReg().ctx().find<FlowContext>()->role == FlowRole::kTitle);
  CHECK(title.CReg().view<const SpriteRenderer>().size() == 1);

  (void)flow.Idle();
  (void)flow.Press(Action::kJump);
  (void)flow.Idle();
  CHECK(flow.Ids() == Ids{"title"});
  (void)flow.Press(Action::kEnter);
  (void)flow.Idle();
  CHECK(flow.Ids() == Ids{"gameplay"});
  CHECK(Top(flow).CReg().ctx().find<FlowContext>()->role ==
        FlowRole::kGameplay);

  testing::FlowHarness other;
  other.Start(kTitleState);
  (void)other.Press(Action::kExit);
  other.ApplyPending();
  // The stack is empty: the runtime ends the game (M13a).
  REQUIRE(other.States().Depth() == 0);
  REQUIRE(other.States().RejectedCount() == 0);
}

TEST_CASE("flow.Pause steps alone over Gameplay, and Exit resumes it",
          "[rampage][flow]") {
  const testing::MainThreadScope main_thread;
  testing::FlowHarness flow;
  flow.Start(kGameplayState);
  for (int i = 0; i < 10; ++i) (void)flow.Idle();
  const Tick gameplay_tick = Top(flow).Clock().tick;
  REQUIRE(gameplay_tick == 10);

  (void)flow.Press(Action::kExit);
  TickResult tick = flow.Idle();
  REQUIRE(flow.Ids() == Ids{"gameplay", "pause"});
  CHECK(tick.top_state == kPauseState);
  REQUIRE(tick.worlds_stepped == 1);
  for (int i = 0; i < 20; ++i) tick = flow.Idle();
  // Pause's World steps; the Gameplay below it does not.
  REQUIRE(Top(flow).Clock().tick == 21);
  const std::vector<StackEntry> entries = flow.States().Entries();
  const World& below = *entries[0].state->Worlds()[0];
  CHECK(below.Clock().tick == gameplay_tick + 1);
  // Both draw: the Gameplay at alpha 1 under the Pause's veil.
  StepPlan plan;
  plan.alpha = 0.25F;
  const std::vector<WorldRenderSource> sources =
      flow.States().RenderSources(plan);
  REQUIRE(sources.size() == 2);
  CHECK(sources[0].world == &below);
  CHECK(sources[0].alpha == 1.0F);
  CHECK(sources[1].world == &Top(flow));
  CHECK(sources[1].alpha == 0.25F);
  // The veil: the wallpaper, dark and half transparent.
  bool veiled = false;
  for (const auto [e, sprite] :
       Top(flow).CReg().view<const SpriteRenderer>().each()) {
    veiled = sprite.tint.a < 255 && sprite.tint.r < 64;
  }
  CHECK(veiled);
  // Jump and Enter do nothing in the Gameplay's World while it is paused.
  (void)flow.Press(Action::kEnter);
  (void)flow.Idle();
  REQUIRE(flow.Ids() == Ids{"gameplay", "pause"});

  (void)flow.Press(Action::kExit);
  tick = flow.Idle();
  REQUIRE(flow.Ids() == Ids{"gameplay"});
  CHECK(tick.top_state == kGameplayState);
  // The same World resumes where it stopped.
  CHECK(&Top(flow) == &below);
  CHECK(Top(flow).Clock().tick == gameplay_tick + 2);
}

TEST_CASE("flow.Pause's Jump returns to the Title with two commands",
          "[rampage][flow]") {
  const testing::MainThreadScope main_thread;
  testing::FlowHarness flow;
  flow.Start(kGameplayState);
  (void)flow.Press(Action::kExit);
  (void)flow.Idle();
  REQUIRE(flow.Ids() == Ids{"gameplay", "pause"});
  const std::uint32_t revision = flow.States().Revision();
  (void)flow.Press(Action::kJump);
  CHECK(flow.Ids() == Ids{"gameplay", "pause"});
  const TickResult tick = flow.Idle();
  // The pop and the replace applied at one boundary, in order: the
  // Gameplay never ticked again.
  CHECK(flow.Ids() == Ids{"title"});
  CHECK(tick.top_state == kTitleState);
  CHECK(flow.States().Revision() == revision + 2);
  REQUIRE(flow.States().RejectedCount() == 0);
}

TEST_CASE("flow.GameOver on the player's destroy request", "[rampage][flow]") {
  const testing::MainThreadScope main_thread;
  testing::FlowHarness flow(testing::kDoomedScene);
  flow.Start(kGameplayState);
  REQUIRE(PlayerAlive(Top(flow)));
  Tick death = 0;
  for (int i = 0; i < 600 && death == 0; ++i) {
    const TickResult tick = flow.Idle();
    REQUIRE(tick.top_state == kGameplayState);
    if (!PlayerAlive(Top(flow))) death = tick.tick;
  }
  REQUIRE(death > 0);
  // The hunter reached the player: the GameOver replaces the Gameplay at
  // the next boundary.
  const TickResult next = flow.Idle();
  CHECK(next.tick == death + 1);
  CHECK(next.top_state == kGameOverState);
  CHECK(flow.Ids() == Ids{"game_over"});

  // A destroy request alone (the player still in the World) is enough.
  testing::FlowHarness requested;
  requested.Start(kGameplayState);
  (void)requested.Idle();
  {
    const InlineOwnerScope owner(OnMainThread());
    World& world = *requested.States().TopWorlds().front();
    const MutationGate gate = world.OpenGate(GateReason::kTest);
    const auto players = world.CReg().view<const PlayerControl>();
    REQUIRE(players.begin() != players.end());
    world.RequestDestroy(*players.begin());
    REQUIRE(!PlayerAlive(world));
    REQUIRE(world.CReg().view<const PlayerControl>().size() == 1);
  }
  (void)requested.Idle();
  (void)requested.Idle();
  CHECK(requested.Ids() == Ids{"game_over"});
}

TEST_CASE("flow.GameOver's Enter retries afresh and Exit returns to the Title",
          "[rampage][flow]") {
  const testing::MainThreadScope main_thread;
  testing::FlowHarness flow(testing::kDoomedScene);
  flow.Start(kGameplayState);
  const std::uint64_t fresh = Top(flow).Hash().Folded();
  while (flow.Ids() != Ids{"game_over"}) {
    REQUIRE(flow.NextTick() < 600);
    (void)flow.Idle();
  }
  const World& over = Top(flow);
  CHECK(over.CReg().ctx().find<FlowContext>()->role == FlowRole::kGameOver);
  for (const auto [e, sprite] :
       over.CReg().view<const SpriteRenderer>().each()) {
    CHECK(sprite.tint.r > sprite.tint.g);
  }
  (void)flow.Press(Action::kEnter);
  flow.ApplyPending();
  REQUIRE(flow.Ids() == Ids{"gameplay"});
  // A fresh World from the scene.
  REQUIRE(Top(flow).Clock().tick == 0);
  CHECK(Top(flow).Hash().Folded() == fresh);
  CHECK(PlayerAlive(Top(flow)));

  while (flow.Ids() != Ids{"game_over"}) {
    REQUIRE(flow.NextTick() < 1200);
    (void)flow.Idle();
  }
  (void)flow.Press(Action::kExit);
  flow.ApplyPending();
  CHECK(flow.Ids() == Ids{"title"});
}

TEST_CASE("flow.The sandbox state's World has no FlowContext",
          "[rampage][flow]") {
  const testing::MainThreadScope main_thread;
  SetSandboxScene(testing::SandboxSceneRequest());
  testing::FlowHarness flow;
  flow.Start(kSandboxState);
  REQUIRE(flow.Ids() == Ids{"sandbox"});
  CHECK(Top(flow).CReg().ctx().find<FlowContext>() == nullptr);
  // No key moves the sandbox: FlowSystem leaves it alone.
  for (const Action action : {Action::kExit, Action::kEnter, Action::kJump}) {
    (void)flow.Press(action);
    (void)flow.Idle();
  }
  CHECK(flow.Ids() == Ids{"sandbox"});
  REQUIRE(flow.States().PendingCount() == 0);
  // The module's states and start states.
  const GameModule& module = RampageModule();
  CHECK(module.start_state == kSandboxState);
  CHECK(module.game_start_state == kTitleState);
  for (const std::string_view id : {kSandboxState, kTitleState, kGameplayState,
                                    kPauseState, kGameOverState}) {
    CHECK(flow.Module().Factory().Contains(id));
  }
}

TEST_CASE("flow.The scenes load the same through content as from files",
          "[rampage][flow]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  const ModuleInstance module(RampageModule());
  const std::unique_ptr<CachedContentSource> content = testing::RepoContent();
  for (const std::string_view scene :
       {kTitleScene, kPauseScene, kGameOverScene, kDefaultGameplayScene,
        testing::kDoomedScene}) {
    INFO(scene);
    World through(module.Registry(), {.name = "content", .seed = 1});
    World from_file(module.Registry(), {.name = "file", .seed = 1});
    SceneRequest request;
    request.path = testing::RampageRepoRoot() + "/" + std::string(scene);
    request.asset_root = testing::RampageAssetRoot();
    request.content_path = std::string(scene);
    {
      const MutationGate gate = through.OpenGate(GateReason::kLoad);
      REQUIRE(LoadRequestedScene(through, request, content.get()).has_value());
    }
    {
      const MutationGate gate = from_file.OpenGate(GateReason::kLoad);
      REQUIRE(LoadRequestedScene(from_file, request).has_value());
    }
    CHECK(through.EntityCount() == from_file.EntityCount());
    CHECK(through.Hash().Folded() == from_file.Hash().Folded());
    REQUIRE(through.EntityCount() > 0);
  }
  // A missing text is an error naming its path.
  SceneRequest missing;
  missing.content_path = "assets/scenes/missing.scene.json";
  World world(module.Registry(), {.name = "missing", .seed = 1});
  const MutationGate gate = world.OpenGate(GateReason::kLoad);
  auto loaded = LoadRequestedScene(world, missing, content.get());
  REQUIRE(!loaded.has_value());
  CHECK(loaded.error().path.find("missing.scene.json") != std::string::npos);
}

TEST_CASE("flow.The sandbox reads through StateEnv::content",
          "[rampage][flow]") {
  const testing::MainThreadScope main_thread;
  SceneRequest request = testing::SandboxSceneRequest();
  // A path that does not exist: only the content source can serve it.
  request.path = "nowhere/sandbox.scene.json";
  request.content_path = std::string(kDefaultGameplayScene);
  SetSandboxScene(request);
  testing::FlowHarness flow;
  flow.Start(kSandboxState);
  REQUIRE(Top(flow).EntityCount() == 17);
  SetSandboxScene(testing::SandboxSceneRequest());
}

TEST_CASE("flow.Scene dependencies are its prefabs under the asset dir",
          "[rampage][flow]") {
  const std::unique_ptr<CachedContentSource> content = testing::RepoContent();
  auto sandbox = content->Text(kDefaultGameplayScene);
  REQUIRE(sandbox.has_value());
  CHECK(SceneDependencies(kDefaultGameplayScene, *sandbox, kContentAssetDir) ==
        Ids{"assets/prefabs/player.prefab.json",
            "assets/prefabs/enemy.prefab.json"});
  auto title = content->Text(kTitleScene);
  REQUIRE(title.has_value());
  CHECK(SceneDependencies(kTitleScene, *title, kContentAssetDir).empty());
  auto prefab = content->Text("assets/prefabs/enemy.prefab.json");
  REQUIRE(prefab.has_value());
  CHECK(SceneDependencies("assets/prefabs/enemy.prefab.json", *prefab,
                          kContentAssetDir)
            .empty());
  CHECK(
      SceneDependencies("x.scene.json", "{not json", kContentAssetDir).empty());
  // Every state's scene and the configured Gameplay scene.
  CHECK(FlowSceneOf(kTitleState) == kTitleScene);
  CHECK(FlowSceneOf(kPauseState) == kPauseScene);
  CHECK(FlowSceneOf(kGameOverState) == kGameOverScene);
  CHECK(FlowSceneOf(kSandboxState).empty());
}

}  // namespace
}  // namespace se::rampage
