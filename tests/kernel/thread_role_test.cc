// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/thread_role.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <thread>

#include "se_core/breadcrumb.h"
#include "support/test_roles.h"

namespace se {
namespace {

TEST_CASE("RegisterMainThread identifies the calling thread", "[thread_role]") {
  testing::MainThreadScope main;
  CHECK(IsMainThread());
  CHECK(CurrentRole() == ThreadRole::kMain);
  AssertMainThread();
  SE_ASSERT_MAIN_THREAD();

  bool other_is_main = true;
  ThreadRole other_role = ThreadRole::kMain;
  std::thread other([&] {
    other_is_main = IsMainThread();
    other_role = CurrentRole();
  });
  other.join();
  CHECK_FALSE(other_is_main);
  CHECK(other_role == ThreadRole::kLibrary);
}

TEST_CASE("Unregistering clears the main thread", "[thread_role]") {
  {
    testing::MainThreadScope main;
    CHECK(IsMainThread());
  }
  CHECK_FALSE(IsMainThread());
  SetCurrentRole(ThreadRole::kLibrary);
}

TEST_CASE("Role names are short and distinct", "[thread_role]") {
  REQUIRE(RoleName(ThreadRole::kMain) == "main");
  REQUIRE(RoleName(ThreadRole::kSim) == "sim");
  REQUIRE(RoleName(ThreadRole::kForkWorker) == "fork");
  REQUIRE(RoleName(ThreadRole::kVblankSampler) == "vblank");
}

TEST_CASE("InlineOwnerScope and WakeMain are callable", "[thread_role]") {
  // The main thread takes the Worlds in inline mode, so the scope needs its
  // proof (death.inline_owner_off_main checks the run-time assert).
  const testing::MainThreadScope main_thread;
  CHECK_FALSE(InlineOwnerScope::HeldOnThisThread());
  {
    const InlineOwnerScope owner(OnMainThread());
    CHECK(InlineOwnerScope::HeldOnThisThread());
  }
  CHECK_FALSE(InlineOwnerScope::HeldOnThisThread());
  WakeMain();
}

TEST_CASE("Breadcrumbs pack the documented fields", "[breadcrumb]") {
  const BreadcrumbFields f{.role = 9,
                           .phase = 6,
                           .system = 0xABC,
                           .chunk = 0xBEEF,
                           .tick = 0x0FEDCBA};
  const std::uint64_t packed = PackBreadcrumb(f);
  REQUIRE(packed == 0x96ABCBEEF0FEDCBAULL);
  CHECK(UnpackBreadcrumb(packed) == f);

  // Each field is truncated to its width.
  const BreadcrumbFields wide{.role = 0x1F,
                              .phase = 0x1F,
                              .system = 0x1FFF,
                              .chunk = 0xFFFF,
                              .tick = 0x1FFFFFFF};
  const BreadcrumbFields back = UnpackBreadcrumb(PackBreadcrumb(wide));
  REQUIRE(back.role == 0xF);
  REQUIRE(back.phase == 0xF);
  REQUIRE(back.system == 0xFFF);
  REQUIRE(back.tick == 0xFFFFFFF);
}

TEST_CASE("Breadcrumb setters touch only their field", "[breadcrumb]") {
  Breadcrumb crumb;
  crumb.SetRole(ThreadRole::kSim);
  crumb.SetPhase(3);
  crumb.SetSystem(17);
  crumb.SetChunk(70000);  // truncated to 16 bits
  crumb.SetTick((1ULL << 28) + 5);
  const BreadcrumbFields f = crumb.Fields();
  CHECK(f.role == static_cast<std::uint8_t>(ThreadRole::kSim));
  REQUIRE(f.phase == 3);
  REQUIRE(f.system == 17);
  CHECK(f.chunk == (70000 & 0xFFFF));
  REQUIRE(f.tick == 5);
  crumb.SetPhase(4);
  REQUIRE(crumb.Fields().system == 17);
  REQUIRE(crumb.Fields().phase == 4);
}

TEST_CASE("Each thread has its own breadcrumb", "[breadcrumb]") {
  ThreadBreadcrumb().SetTick(11);
  std::uint32_t other_tick = 99;
  std::thread other([&] { other_tick = ThreadBreadcrumb().Fields().tick; });
  other.join();
  REQUIRE(other_tick == 0);
  REQUIRE(ThreadBreadcrumb().Fields().tick == 11);
  ThreadBreadcrumb().SetTick(0);
}

}  // namespace
}  // namespace se
