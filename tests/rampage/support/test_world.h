// Copyright 2026 The simple-engine-2 Authors.
//
// A rampage World for unit tests: the module's registry and Schedule, scene
// settings in the ctx, entities added by the test, and Step() to run ticks
// with a chosen input. Construct it inside a MainThreadScope and an
// InlineOwnerScope.

#ifndef TESTS_RAMPAGE_SUPPORT_TEST_WORLD_H_
#define TESTS_RAMPAGE_SUPPORT_TEST_WORLD_H_

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
#include "se_sim/game_module.h"

namespace se::testing {

class RampageTestWorld {
 public:
  explicit RampageTestWorld(
      const rampage::SceneSettings& settings = rampage::SceneSettings{})
      SE_REQUIRES(g_sim_owner)
      : module_(rampage::RampageModule()),
        world_(std::make_unique<World>(module_.Registry(),
                                       WorldDesc{.name = "test"})) {
    rampage::ApplySceneSettings(*world_, settings);
  }

  [[nodiscard]] World& Get() noexcept { return *world_; }
  [[nodiscard]] entt::registry& Reg() SE_REQUIRES(g_sim_owner) {
    return world_->Reg();
  }
  [[nodiscard]] rampage::StatNames& Names() SE_REQUIRES(g_sim_owner) {
    return world_->Reg().ctx().get<rampage::StatNames>();
  }
  [[nodiscard]] const ComponentRegistry& Registry() const noexcept {
    return module_.Registry();
  }

  // An entity with a Transform, a collider of half extents |half| and a
  // team; the next Guid {7, n}.
  entt::entity Add(Vec2 pos, Vec2 half = {24.0F, 24.0F},
                   rampage::TeamId team = rampage::TeamId::kEnemy)
      SE_REQUIRES(g_sim_owner) {
    const entt::entity e = world_->Create(Guid{7, ++next_guid_});
    entt::registry& reg = world_->Reg();
    reg.emplace<Transform>(e, pos, 0.0F, Vec2{1.0F, 1.0F}, Tick{0});
    reg.emplace<rampage::Collider>(e, half);
    reg.emplace<rampage::Team>(e, team);
    return e;
  }

  // Stats from (name, value) pairs.
  void SetStat(entt::entity e, std::string_view name, float value)
      SE_REQUIRES(g_sim_owner) {
    const rampage::StatId id = Names().Add(name);
    rampage::SetStat(Reg().get_or_emplace<rampage::Stats>(e), id, value);
  }

  [[nodiscard]] const float* Stat(entt::entity e, std::string_view name) const {
    const auto* stats = world_->CReg().try_get<rampage::Stats>(e);
    return stats == nullptr
               ? nullptr
               : rampage::FindStat(*stats, rampage::StatIdOf(name));
  }

  // FinishLoad: derived components and FinalizeSpawns.
  void Finish() SE_REQUIRES(g_sim_owner) { rampage::FinishLoad(*world_); }

  // Runs |ticks| ticks with |held| action bits.
  void Step(int ticks = 1, std::uint64_t held = 0) SE_REQUIRES(g_sim_owner) {
    for (int i = 0; i < ticks; ++i) {
      InputFrame frame;
      frame.tick = world_->Clock().tick + 1;
      frame.held = held;
      module_.MutableSchedule().Step(*world_, frame, pool_);
    }
  }

  [[nodiscard]] Tick Now() const noexcept { return world_->Clock().tick; }
  [[nodiscard]] bool Alive(entt::entity e) const {
    return world_->CReg().valid(e);
  }

 private:
  ModuleInstance module_;
  std::unique_ptr<World> world_;
  jobs::ForkJoinPool pool_;
  std::uint64_t next_guid_ = 0;
};

}  // namespace se::testing

#endif  // TESTS_RAMPAGE_SUPPORT_TEST_WORLD_H_
