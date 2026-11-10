// Copyright 2026 The simple-engine-2 Authors.
//
// A World for the panel tests: rampage's module registry (core, render and
// rampage components), built by the test, of any WorldKind. Play Worlds
// keep their MutationGate closed except inside Edit(), so a panel that
// mutated the World outside a command would trip the gate's assert.
// Construct it inside a MainThreadScope and an InlineOwnerScope.

#ifndef TESTS_EDITOR_PANELS_SUPPORT_PANEL_WORLD_H_
#define TESTS_EDITOR_PANELS_SUPPORT_PANEL_WORLD_H_

#include <cstdint>
#include <entt/entity/registry.hpp>
#include <memory>
#include <string_view>

#include "game_rampage/components.h"
#include "game_rampage/module.h"
#include "game_rampage/scene.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_sim/game_module.h"

namespace se::testing {

class PanelWorld {
 public:
  explicit PanelWorld(WorldKind kind = WorldKind::kEditDocument)
      SE_REQUIRES(g_sim_owner)
      : module_(rampage::RampageModule()),
        world_(std::make_unique<World>(
            module_.Registry(),
            WorldDesc{.name = "panel-test", .kind = kind})) {
    auto gate = Gate();
    rampage::ApplySceneSettings(*world_, rampage::SceneSettings{});
  }

  [[nodiscard]] World& Get() noexcept { return *world_; }
  [[nodiscard]] const World& CGet() const noexcept { return *world_; }
  [[nodiscard]] const ComponentRegistry& Registry() const noexcept {
    return module_.Registry();
  }

  // A sprite entity: Transform at |pos|, a w x h SpriteRenderer centred on
  // it on |layer| at |z|, Guid {9, n}.
  entt::entity AddSprite(Vec2 pos, float w = 48.0F, float h = 48.0F,
                         std::int16_t layer = 0, float z = 0.0F,
                         std::uint32_t texture = TextureKeyOf("player"))
      SE_REQUIRES(g_sim_owner) {
    auto gate = Gate();
    const entt::entity e = world_->Create(Guid{9, ++next_guid_});
    entt::registry& reg = world_->Reg();
    reg.emplace<Transform>(e, pos, 0.0F, Vec2{1.0F, 1.0F}, Tick{0});
    SpriteRenderer sprite;
    sprite.texture = texture;
    sprite.region = Rect{0.0F, 0.0F, w, h};
    sprite.origin = Vec2{w / 2.0F, h / 2.0F};
    sprite.layer = layer;
    sprite.z = z;
    reg.emplace<SpriteRenderer>(e, sprite);
    return e;
  }

  // FinishLoad (derived components, FinalizeSpawns).
  void Finish() SE_REQUIRES(g_sim_owner) {
    auto gate = Gate();
    rampage::FinishLoad(*world_);
  }

  // Runs |ticks| ticks of rampage's Schedule (it opens its own gate).
  void Step(int ticks = 1) SE_REQUIRES(g_sim_owner) {
    for (int i = 0; i < ticks; ++i) {
      InputFrame frame;
      frame.tick = world_->Clock().tick + 1;
      module_.MutableSchedule().Step(*world_, frame, pool_);
    }
  }

  [[nodiscard]] Guid GuidOf(entt::entity e) const {
    return world_->CReg().get<IdComponent>(e).guid;
  }

 private:
  [[nodiscard]] MutationGate Gate() {
    return world_->OpenGate(GateReason::kTest);
  }

  ModuleInstance module_;
  std::unique_ptr<World> world_;
  jobs::ForkJoinPool pool_;
  std::uint64_t next_guid_ = 0;
};

}  // namespace se::testing

#endif  // TESTS_EDITOR_PANELS_SUPPORT_PANEL_WORLD_H_
