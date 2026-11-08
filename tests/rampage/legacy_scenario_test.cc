// Copyright 2026 The simple-engine-2 Authors.
//
// The legacy scenario loader: the three reference scenarios load into the
// World shape the systems expect (the Sandbox::init player first, then the
// entities in file order), colliders come from the PNG IHDR sizes, and bad
// scenarios are errors.

#include "game_rampage/legacy_scenario.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/module.h"
#include "game_rampage/systems.h"
#include "rampage/support/rampage_session.h"
#include "rampage/support/test_world.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "support/test_roles.h"

#ifndef SE_TEST_OUT_DIR
#error "SE_TEST_OUT_DIR must name a writable directory"
#endif

namespace se::rampage {
namespace {

std::string Scenario(const std::string& name) {
  return testing::RampageFixtureDir() + "/reference/" + name + ".scenario.json";
}

LegacyScenarioOptions Options() {
  LegacyScenarioOptions options;
  options.root = testing::RampageRepoRoot();
  return options;
}

// The entity with spawn serial |serial|.
entt::entity BySerial(const World& world, std::uint32_t serial) {
  for (const auto [e, spawned] : world.CReg().view<const Spawned>().each()) {
    if (spawned.spawn_serial == serial) return e;
  }
  return entt::null;
}

TEST_CASE("PNG sizes come from the IHDR chunk", "[rampage][legacy]") {
  const auto player =
      ReadPngSize(testing::RampageRepoRoot() + "/assets/player/player.png");
  REQUIRE(player.has_value());
  REQUIRE(player->width == 48);
  REQUIRE(player->height == 48);
  const auto coin =
      ReadPngSize(testing::RampageRepoRoot() + "/assets/coin/coin.png");
  REQUIRE(coin.has_value());
  REQUIRE(coin->width == 18);
  REQUIRE(coin->height == 19);
  CHECK_FALSE(ReadPngSize(Scenario("stress")).has_value());
  CHECK_FALSE(ReadPngSize("no/such/file.png").has_value());
}

TEST_CASE("The reference scenarios load", "[rampage][legacy]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  for (const auto& [name, count] :
       std::vector<std::pair<std::string, std::size_t>>{
           {"stress", 17}, {"effects", 16}, {"patrol", 21}}) {
    INFO(name);
    testing::RampageTestWorld w;
    const auto loaded = LoadLegacyScenario(w.Get(), Scenario(name), Options());
    REQUIRE(loaded.has_value());
    CHECK(loaded->name == name);
    CHECK(loaded->entities == count);
    CHECK(w.Get().EntityCount() == count);
    // The player is spawn serial 0 and has Sandbox::init's components.
    const entt::entity player = BySerial(w.Get(), 0);
    REQUIRE(player != entt::entity{entt::null});
    const entt::registry& reg = w.Get().CReg();
    CHECK(reg.get<Team>(player).id == TeamId::kFriendly);
    CHECK(reg.get<Collider>(player).half_extents == Vec2{24.0F, 24.0F});
    CHECK(reg.get<PlayerControl>(player).speed == 120.0F);
    CHECK(reg.get<EffectSource>(player).polled);
    CHECK(reg.get<EffectSource>(player).effects.size() == 3);
    CHECK(reg.all_of<PairCooldowns, ActiveEffects, MoveIntent, KinematicBody>(
        player));
    // Every scenario entity has Stats (an EntityStatus), a team and a
    // collider.
    for (std::uint32_t s = 1; s < count; ++s) {
      const entt::entity e = BySerial(w.Get(), s);
      REQUIRE(e != entt::entity{entt::null});
      CHECK(reg.all_of<Stats, ActiveEffects, Team, Collider, Transform>(e));
    }
    CHECK(reg.get<IdComponent>(player).guid == LegacyGuid(name, 0));
  }
}

TEST_CASE("The effects scenario's overrides and entity forms",
          "[rampage][legacy]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  REQUIRE(LoadLegacyScenario(w.Get(), Scenario("effects"), Options()));
  const entt::registry& reg = w.Get().CReg();
  const auto& settings = reg.ctx().get<EffectSettings>();
  REQUIRE(settings.event_watchdog_ms == 2000);
  CHECK(reg.ctx().get<ArenaBounds>().w == 1920.0F);

  const entt::entity player = BySerial(w.Get(), 0);
  CHECK(reg.get<Transform>(player).pos == Vec2{300.0F, 540.0F});
  const auto& source = reg.get<EffectSource>(player);
  REQUIRE(source.cooldown_ms == 500);
  CHECK(source.effects[1].kind == EffectKind::kOverTime);
  REQUIRE(source.effects[1].interval_ms == 250);
  REQUIRE(source.effects[1].duration_ms == 1100);
  CHECK(w.Stat(player, "HP") != nullptr);
  CHECK(*w.Stat(player, "Charge") == 10000000.0F);

  // A stationary receiver: no flag, no path, no source.
  const entt::entity still = BySerial(w.Get(), 1);
  CHECK_FALSE(reg.all_of<Moved>(still));
  CHECK_FALSE(reg.all_of<Path>(still));
  CHECK_FALSE(reg.all_of<EffectSource>(still));
  // A path entity starts at its first point, flagged as moving.
  const entt::entity walker = BySerial(w.Get(), 2);
  CHECK(reg.get<Transform>(walker).pos == Vec2{700.0F, 300.0F});
  CHECK(reg.all_of<Moved, Path, PathCache>(walker));
  CHECK(reg.get<Path>(walker).loop);
  // The polled turret that is not ready on start.
  const auto& turret = reg.get<EffectSource>(BySerial(w.Get(), 3));
  CHECK(turret.polled);
  CHECK_FALSE(turret.ready_on_start);
  REQUIRE(turret.cooldown_ms == 5500);
  // The rotated collider: the rotation is kept, the collider is not turned.
  const entt::entity rotated = BySerial(w.Get(), 7);
  CHECK(reg.get<Transform>(rotated).rot == RotationFromHeading(45.0F));
  CHECK(reg.get<Collider>(rotated).half_extents == Vec2{24.0F, 24.0F});
  // A non-repeating path.
  CHECK_FALSE(reg.get<Path>(BySerial(w.Get(), 10)).loop);
}

// Loads a scenario from |text| (written to a file first).
std::expected<LegacyScenarioSummary, Error> LoadText(const std::string& text)
    SE_REQUIRES(g_sim_owner) {
  const std::string path = std::string(SE_TEST_OUT_DIR) + "/bad.scenario.json";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
  }
  testing::RampageTestWorld w;
  return LoadLegacyScenario(w.Get(), path, Options());
}

TEST_CASE("Bad scenarios are errors", "[rampage][legacy]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  const std::string dir = SE_TEST_OUT_DIR;
  CHECK_FALSE(LoadText("{\"format\": \"something-else\"}"));
  CHECK_FALSE(LoadText("{\"entities\": [{\"team\": \"PIRATES\"}]}"));
  CHECK_FALSE(LoadText("{\"entities\": [{\"texture\": \"nope\"}]}"));
  CHECK_FALSE(LoadText("{\"entities\": [{\"bogus\": 1}]}"));
  CHECK_FALSE(LoadText("{\"player\": {\"polling\": {\"cooldownMs\": 1.5}}}"));
  CHECK_FALSE(LoadText("{\"entities\": [{\"path\": [[1, 2, 3]]}]}"));
  const auto good = LoadText(
      "{\"format\": \"se-legacy-scenario-v1\", \"name\": \"x\", "
      "\"entities\": [{\"team\": \"UNAFFILIATED\", \"texture\": \"coin\", "
      "\"effects\": [{\"type\": \"NULLTYPE\"}, {\"type\": \"TEMPTIMED\"}]}]}");
  REQUIRE(good.has_value());
  REQUIRE(good->entities == 2);
  CHECK_FALSE(LoadText("not json"));
  testing::RampageTestWorld w;
  CHECK_FALSE(LoadLegacyScenario(w.Get(), dir + "/missing.json", Options()));
}

}  // namespace
}  // namespace se::rampage
