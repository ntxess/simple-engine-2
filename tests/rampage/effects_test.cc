// Copyright 2026 The simple-engine-2 Authors.
//
// Effects and contacts (docs/rules/gameplay-rules.md): R1 (TEMPTIMED always
// reverts), R2 (per-pair cooldowns, readyOnStart, self and same-team skips),
// R7 (OVERTIME timing and count), the event watchdog, a receiver destroyed
// mid-effect, and R8 (contact order shows in non-integer sums).

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/scene.h"
#include "rampage/support/test_world.h"
#include "se_core/thread_role.h"
#include "support/test_roles.h"

namespace se::rampage {
namespace {

using testing::RampageTestWorld;

SceneSettings WithWatchdog(std::int64_t ms) {
  SceneSettings settings;
  settings.effects.event_watchdog_ms = ms;
  return settings;
}

EffectDef Effect(RampageTestWorld& w, EffectKind kind, float value,
                 std::int64_t duration_ms = 0, std::int64_t interval_ms = 0)
    SE_REQUIRES(g_sim_owner) {
  return EffectDef{kind, w.Names().Add("HP"), value, duration_ms, interval_ms};
}

// A receiver with HP 100 at (500, 500).
entt::entity Receiver(RampageTestWorld& w, float hp = 100.0F,
                      TeamId team = TeamId::kEnemy) SE_REQUIRES(g_sim_owner) {
  const entt::entity e = w.Add({500.0F, 500.0F}, {24.0F, 24.0F}, team);
  w.SetStat(e, "HP", hp);
  return e;
}

// An event-driven source overlapping the receiver that moved once, at load:
// one contact, on tick 1.
entt::entity OneShotSource(RampageTestWorld& w, std::vector<EffectDef> effects,
                           Vec2 pos = {510.0F, 500.0F})
    SE_REQUIRES(g_sim_owner) {
  const entt::entity e = w.Add(pos, {24.0F, 24.0F}, TeamId::kFriendly);
  EffectSource source;
  source.effects = std::move(effects);
  w.Reg().emplace<EffectSource>(e, std::move(source));
  w.Reg().emplace<Moved>(e);
  return e;
}

entt::entity PolledSource(RampageTestWorld& w, std::vector<EffectDef> effects,
                          std::int64_t cooldown_ms, bool ready_on_start,
                          Vec2 pos = {510.0F, 500.0F},
                          TeamId team = TeamId::kFriendly)
    SE_REQUIRES(g_sim_owner) {
  const entt::entity e = w.Add(pos, {24.0F, 24.0F}, team);
  EffectSource source;
  source.effects = std::move(effects);
  source.polled = true;
  source.cooldown_ms = cooldown_ms;
  source.ready_on_start = ready_on_start;
  w.Reg().emplace<EffectSource>(e, std::move(source));
  return e;
}

float Hp(const RampageTestWorld& w, entt::entity e) {
  const float* hp = w.Stat(e, "HP");
  REQUIRE(hp != nullptr);
  return *hp;
}

// The ticks at which the receiver's HP changed, over |ticks| ticks.
std::vector<Tick> ChangeTicks(RampageTestWorld& w, entt::entity receiver,
                              int ticks) SE_REQUIRES(g_sim_owner) {
  std::vector<Tick> changes;
  float before = Hp(w, receiver);
  for (int i = 0; i < ticks; ++i) {
    w.Step();
    const float now = Hp(w, receiver);
    if (now != before) changes.push_back(w.Now());
    before = now;
  }
  return changes;
}

TEST_CASE("R7: OVERTIME 250 ms over 1100 ms applies 4 times, 30 ticks apart",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  const entt::entity r = Receiver(w);
  OneShotSource(w, {Effect(w, EffectKind::kOverTime, -2.0F, 1100, 250)});
  w.Finish();
  CHECK(ChangeTicks(w, r, 400) == std::vector<Tick>{31, 61, 91, 121});
  CHECK(Hp(w, r) == 92.0F);
  CHECK(w.Reg().get<ActiveEffects>(r).effects.empty());
}

TEST_CASE("R7: OVERTIME 1000 ms over 5000 ms applies 5 times, 120 apart",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  const entt::entity r = Receiver(w);
  OneShotSource(w, {Effect(w, EffectKind::kOverTime, -1.0F, 5000, 1000)});
  w.Finish();
  CHECK(ChangeTicks(w, r, 800) == std::vector<Tick>{121, 241, 361, 481, 601});
  CHECK(Hp(w, r) == 95.0F);
}

TEST_CASE("R7: an interval of 0 applies once per tick for ticks(d) ticks",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  const entt::entity r = Receiver(w);
  // ticks(50 ms) = 6, starting one tick after the contact.
  OneShotSource(w, {Effect(w, EffectKind::kOverTime, -1.0F, 50, 0)});
  w.Finish();
  CHECK(ChangeTicks(w, r, 20) == std::vector<Tick>{2, 3, 4, 5, 6, 7});
  CHECK(Hp(w, r) == 94.0F);
}

TEST_CASE("R7: a duration shorter than the interval applies nothing",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  const entt::entity r = Receiver(w);
  OneShotSource(w, {Effect(w, EffectKind::kOverTime, -1.0F, 100, 250)});
  w.Finish();
  w.Step();
  // It completes on the contact tick.
  CHECK(w.Reg().get<ActiveEffects>(r).effects.empty());
  CHECK(ChangeTicks(w, r, 100).empty());
  CHECK(Hp(w, r) == 100.0F);
}

TEST_CASE("R1: TEMPTIMED applies at contact and reverts at its end",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  const entt::entity r = Receiver(w);
  OneShotSource(w, {Effect(w, EffectKind::kTempTimed, -10.0F, 5000)});
  w.Finish();
  CHECK(ChangeTicks(w, r, 700) == std::vector<Tick>{1, 601});
  CHECK(Hp(w, r) == 100.0F);
}

TEST_CASE("R1: the watchdog ends a TEMPTIMED early and reverts it",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  // 2 s = 240 ticks; the effect lasts 40 s.
  RampageTestWorld w(WithWatchdog(2000));
  const entt::entity r = Receiver(w);
  OneShotSource(w, {Effect(w, EffectKind::kTempTimed, -5.0F, 40000)});
  w.Finish();
  w.Step(241);
  CHECK(Hp(w, r) == 95.0F);
  w.Step();  // tick 242: more than 240 ticks since the application
  CHECK(Hp(w, r) == 100.0F);
  CHECK(w.Reg().get<ActiveEffects>(r).effects.empty());
}

TEST_CASE("R7: the watchdog ends an OVERTIME whose interval exceeds it",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w(WithWatchdog(2000));
  const entt::entity r = Receiver(w);
  // ticks(3000 ms) = 360 > 240: it ends at tick 242 without applying.
  OneShotSource(w, {Effect(w, EffectKind::kOverTime, -1.0F, 9000, 3000)});
  w.Finish();
  w.Step(241);
  CHECK(w.Reg().get<ActiveEffects>(r).effects.size() == 1);
  w.Step();
  CHECK(w.Reg().get<ActiveEffects>(r).effects.empty());
  CHECK(ChangeTicks(w, r, 600).empty());
  CHECK(Hp(w, r) == 100.0F);
}

TEST_CASE("The watchdog lets the application of its last tick happen",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  // Watchdog 1000 ms = 120 ticks: the effect ends once more than 120 ticks
  // have passed since its last application, and the check comes after the
  // tick's application.
  RampageTestWorld w(WithWatchdog(1000));
  const entt::entity r = Receiver(w);
  // ticks(1009) = ceil(121.08) = 122 > 121: ended before the application.
  OneShotSource(w, {Effect(w, EffectKind::kOverTime, -1.0F, 5000, 1009)});
  w.Finish();
  CHECK(ChangeTicks(w, r, 300).empty());

  // ticks(1008) = ceil(120.96) = 121: the application on tick 122 comes
  // first, then the watchdog (121 > 120) ends the effect.
  RampageTestWorld v(WithWatchdog(1000));
  const entt::entity r2 = Receiver(v);
  OneShotSource(v, {Effect(v, EffectKind::kOverTime, -1.0F, 5000, 1008)});
  v.Finish();
  CHECK(ChangeTicks(v, r2, 300) == std::vector<Tick>{122});
}

TEST_CASE("An effect is dropped with its receiver", "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  // HP 25, -10 every second: 15, 5, then -5 at tick 361 and destroyed.
  const entt::entity r = Receiver(w, 25.0F);
  const entt::entity other = Receiver(w, 100.0F);
  w.Reg().get<Transform>(other).pos = {520.0F, 500.0F};
  OneShotSource(w, {Effect(w, EffectKind::kOverTime, -10.0F, 5000, 1000)});
  w.Finish();
  w.Step(360);
  REQUIRE(w.Alive(r));
  CHECK(Hp(w, r) == 5.0F);
  w.Step();
  CHECK_FALSE(w.Alive(r));
  // The other receiver's copy of the effect runs on to its end.
  w.Step(300);
  CHECK(Hp(w, other) == 50.0F);
  CHECK(w.Reg().get<ActiveEffects>(other).effects.empty());
}

TEST_CASE("A receiver destroyed mid-TEMPTIMED is never reverted",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  const entt::entity r = Receiver(w, 100.0F);
  OneShotSource(w, {Effect(w, EffectKind::kTempTimed, -10.0F, 5000)});
  w.Finish();
  w.Step(100);
  CHECK(Hp(w, r) == 90.0F);
  w.Get().RequestDestroy(r);
  w.Get().DrainDestroyRequests();
  w.Step(600);
  CHECK_FALSE(w.Alive(r));
  CHECK(w.Reg().view<ActiveEffects>().empty());
}

TEST_CASE("R2: a polled source ready on start hits at once, then per cooldown",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  const entt::entity r = Receiver(w);
  // 1000 ms = 120 ticks: a contact again once more than 120 ticks passed.
  const entt::entity s =
      PolledSource(w, {Effect(w, EffectKind::kInstant, -10.0F)}, 1000, true);
  w.Finish();
  CHECK(ChangeTicks(w, r, 400) == std::vector<Tick>{1, 122, 243, 364});
  const auto& pairs = w.Reg().get<PairCooldowns>(s).pairs;
  REQUIRE(pairs.size() == 1);
  CHECK(pairs[0].receiver == w.Reg().get<IdComponent>(r).guid);
  REQUIRE(pairs[0].last == 364);
}

TEST_CASE("R2: a source not ready on start waits one cooldown from spawn",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  const entt::entity r = Receiver(w);
  PolledSource(w, {Effect(w, EffectKind::kInstant, -10.0F)}, 1000, false);
  w.Finish();
  // Spawned at tick 0: first more than 120 ticks later, at 121.
  CHECK(ChangeTicks(w, r, 300) == std::vector<Tick>{121, 242});
}

TEST_CASE("R2: one source keeps a cooldown per receiver",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  const entt::entity a = Receiver(w);
  const entt::entity b = Receiver(w);
  w.Reg().get<Transform>(b).pos = {530.0F, 510.0F};
  // A third receiver enters the source's reach later, and is hit on the
  // tick it arrives even though the source hit the others then.
  const entt::entity c = Receiver(w);
  w.Reg().get<Transform>(c).pos = {900.0F, 900.0F};
  PolledSource(w, {Effect(w, EffectKind::kInstant, -10.0F)}, 1000, true);
  w.Finish();
  w.Step();
  CHECK(Hp(w, a) == 90.0F);
  CHECK(Hp(w, b) == 90.0F);
  CHECK(Hp(w, c) == 100.0F);
  w.Step(9);
  w.Reg().get<Transform>(c).pos = {520.0F, 520.0F};
  w.Step();
  CHECK(Hp(w, c) == 90.0F);
  CHECK(Hp(w, a) == 90.0F);
}

TEST_CASE("R2: self and same-team pairs are skipped before the cooldown",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  const entt::entity friend_rcv = Receiver(w, 100.0F, TeamId::kFriendly);
  const entt::entity s =
      PolledSource(w, {Effect(w, EffectKind::kInstant, -10.0F)}, 1000, true);
  // The source has stats of its own, so a self contact could change them.
  w.SetStat(s, "HP", 100.0F);
  w.Finish();
  w.Step(10);
  CHECK(Hp(w, friend_rcv) == 100.0F);
  CHECK(Hp(w, s) == 100.0F);
  // Neither pair used the cooldown: nothing is recorded.
  CHECK(w.Reg().get<PairCooldowns>(s).pairs.empty());
  // An enemy arriving now is hit at once.
  const entt::entity enemy = Receiver(w);
  w.Reg().emplace<ActiveEffects>(enemy);
  w.Step();
  CHECK(Hp(w, enemy) == 90.0F);
}

TEST_CASE("Event-driven sources act only on ticks they moved",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  const entt::entity r = Receiver(w);
  const entt::entity s =
      OneShotSource(w, {Effect(w, EffectKind::kInstant, -1.0F)});
  w.Finish();
  w.Step(5);
  CHECK(Hp(w, r) == 99.0F);
  // Moving it again (no cooldown) hits again on that tick.
  w.Reg().emplace<KinematicBody>(s, Vec2{12.0F, 0.0F});
  w.Step(3);
  CHECK(Hp(w, r) == 96.0F);
}

TEST_CASE("R8: contacts apply in (source, receiver) spawn order",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  // Charge 1e7, where a float's spacing is 1: +1.5 then +0.75 rounds to
  // 10000003, +0.75 then +1.5 to 10000002.
  for (const bool big_first : {true, false}) {
    INFO("the +1.5 source spawned first: " << big_first);
    RampageTestWorld w;
    const entt::entity r = Receiver(w);
    w.SetStat(r, "Charge", 1.0e7F);
    const StatId charge = w.Names().Add("Charge");
    const EffectDef big{EffectKind::kInstant, charge, 1.5F, 0, 0};
    const EffectDef small{EffectKind::kInstant, charge, 0.75F, 0, 0};
    OneShotSource(w, {big_first ? big : small});
    OneShotSource(w, {big_first ? small : big}, {490.0F, 500.0F});
    w.Finish();
    w.Step();
    const float* value = w.Stat(r, "Charge");
    REQUIRE(value != nullptr);
    CHECK(*value == (big_first ? 10000003.0F : 10000002.0F));
  }
}

TEST_CASE("Effects of one contact apply in list order, earlier ticks first",
          "[rampage][effects]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  RampageTestWorld w;
  const entt::entity r = Receiver(w);
  const entt::entity s =
      PolledSource(w,
                   {Effect(w, EffectKind::kInstant, -10.0F),
                    Effect(w, EffectKind::kOverTime, -1.0F, 5000, 1000),
                    Effect(w, EffectKind::kTempTimed, -10.0F, 5000)},
                   1000, true);
  w.Finish();
  w.Step();
  const auto& active = w.Reg().get<ActiveEffects>(r).effects;
  // The INSTANT completed; the other two run, in creation order.
  REQUIRE(active.size() == 2);
  CHECK(active[0].def.kind == EffectKind::kOverTime);
  CHECK(active[1].def.kind == EffectKind::kTempTimed);
  CHECK(active[0].seq < active[1].seq);
  CHECK(active[0].source.guid == w.Reg().get<IdComponent>(s).guid);
  REQUIRE(active[0].next == 121);
  REQUIRE(active[1].next == 601);
  CHECK(Hp(w, r) == 80.0F);
  w.Step(121);
  // Tick 122: the second contact's effects come after the first one's.
  REQUIRE(active.size() == 4);
  CHECK(active[1].seq < active[2].seq);
}

}  // namespace
}  // namespace se::rampage
