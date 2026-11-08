// Copyright 2026 The simple-engine-2 Authors.
//
// The projection writer emits the legacy .seproj text
// (core/util/SemanticProjection.cpp): spawn-serial order, %.9g values, the
// rotation in degrees in [0, 360), HP first and the other stats by name,
// "-" for absent values, sanitised stat names and FNV-1a 64 tick hashes.

#include "game_rampage/projection.h"

#include <catch2/catch_test_macros.hpp>
#include <entt/entity/registry.hpp>
#include <numbers>
#include <string>

#include "game_rampage/components.h"
#include "game_rampage/systems.h"
#include "rampage/support/test_world.h"
#include "se_core/core_components.h"
#include "se_core/thread_role.h"
#include "support/test_roles.h"

namespace se::rampage {
namespace {

TEST_CASE("Projection lines follow the legacy format",
          "[rampage][projection]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  const entt::entity a = w.Add({200.0F, 200.0F});
  w.SetStat(a, "HP", 100.0F);
  w.SetStat(a, "Speed", 500.0F);
  w.SetStat(a, "Charge", 1.0e7F);
  w.SetStat(a, "max hp=", 0.1F);
  const entt::entity b = w.Add({302.083344F, -0.5F});
  w.Reg().get<Transform>(b).rot = RotationFromHeading(180.0F);
  // Stats without HP.
  w.SetStat(b, "Speed", 250.0F);
  // No Transform and no Stats.
  static_cast<void>(w.Get().Create(Guid{9, 9}));
  w.Finish();

  std::string block;
  WriteProjection(w.Get(), block);
  REQUIRE(block ==
          "t 0\n"
          "0 200 200 0 100 Charge=10000000 Speed=500 max_hp_=0.100000001\n"
          "1 302.083344 -0.5 180 - Speed=250\n"
          "2 - - - -\n");
  CHECK(Fnv1a64(kFnvOffsetBasis, "") == kFnvOffsetBasis);
  REQUIRE(Fnv1a64(kFnvOffsetBasis, "a") == 0xaf63dc4c8601ec8cULL);
}

TEST_CASE("Rotations print in degrees in [0, 360)", "[rampage][projection]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  const entt::entity e = w.Add({0.0F, 0.0F});
  w.Finish();
  const auto line = [&w]() {
    std::string block;
    WriteProjection(w.Get(), block);
    return block.substr(block.find('\n') + 1);
  };
  // The sf::Angle conversions: a heading of -90 degrees wraps to 270.
  w.Reg().get<Transform>(e).rot = RotationFromHeading(-90.0F);
  REQUIRE(line() == "0 0 0 270 -\n");
  w.Reg().get<Transform>(e).rot = RotationFromHeading(45.0F);
  REQUIRE(line() == "0 0 0 45 -\n");
  // A full turn (which the wrap can round to) prints as 0.
  w.Reg().get<Transform>(e).rot = 2.0F * 3.141592654F;
  REQUIRE(line() == "0 0 0 0 -\n");
}

TEST_CASE("Projection files carry the tick hash and the run hash",
          "[rampage][projection]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  w.Add({1.0F, 2.0F});
  w.Finish();
  ProjectionFile file;
  file.Write(w.Get());
  std::string block;
  WriteProjection(w.Get(), block);
  CHECK(file.TickHash() == Fnv1a64(kFnvOffsetBasis, block));
  CHECK(file.RunHash() == file.TickHash());
  w.Step();
  file.Write(w.Get());
  std::string next;
  WriteProjection(w.Get(), next);
  CHECK(file.RunHash() == Fnv1a64(Fnv1a64(kFnvOffsetBasis, block), next));
  REQUIRE(file.Ticks() == 2);
}

}  // namespace
}  // namespace se::rampage
