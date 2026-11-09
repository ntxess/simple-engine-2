// Copyright 2026 The simple-engine-2 Authors.

#include "se_os/thread.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <latch>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "se_core/breadcrumb.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_os/cpu_topology.h"
#include "support/test_roles.h"

namespace se {
namespace {

TEST_CASE("StartThread names the thread and gives it its role and a slot",
          "[thread]") {
  std::latch started(1);
  std::latch checked(1);
  std::optional<std::uint32_t> slot;
  ThreadRole role = ThreadRole::kLibrary;
  std::string inside_name;
  std::uint64_t os_id = 0;
  auto thread =
      StartThread({.role = ThreadRole::kBackground, .name = "se-bg-lane"}, [&] {
        slot = CurrentThreadSlot();
        role = CurrentRole();
        os_id = CurrentOsThreadId();
        inside_name = CurrentOsThreadName().value_or("?");
        started.count_down();
        checked.wait();
      });
  REQUIRE(thread.has_value());
  started.wait();
  // Read back from outside while the thread runs: the OS name
  // (GetThreadDescription / pthread_getname_np) and the registry slot.
  const std::expected<std::string, Error> outside = OsThreadName(*thread);
  REQUIRE(outside.has_value());
  REQUIRE(*outside == "se-bg-lane");
  REQUIRE(inside_name == "se-bg-lane");
  CHECK(role == ThreadRole::kBackground);
  REQUIRE(slot.has_value());
  CHECK(*slot != kMainThreadSlot);
  const ThreadSlotInfo info = ThreadRegistry::Instance().Read(*slot);
  CHECK(info.state == ThreadSlotState::kLive);
  CHECK(info.role == ThreadRole::kBackground);
  REQUIRE(info.Name() == "se-bg-lane");
  CHECK(info.os_thread_id == os_id);
  REQUIRE(os_id != 0);
  CHECK(UnpackBreadcrumb(info.breadcrumb).role ==
        static_cast<std::uint8_t>(ThreadRole::kBackground));
  checked.count_down();
  thread->Join();
  CHECK(ThreadRegistry::Instance().Read(*slot).state ==
        ThreadSlotState::kExited);
}

TEST_CASE("Linux keeps 15 characters of a long OS thread name", "[thread]") {
  std::string inside;
  auto thread = StartThread(
      {.role = ThreadRole::kBackground, .name = "a-very-long-thread-name"},
      [&] { inside = CurrentOsThreadName().value_or("?"); });
  REQUIRE(thread.has_value());
  thread->Join();
#if defined(_WIN32)
  REQUIRE(inside == "a-very-long-thread-name");
#else
  REQUIRE(inside == "a-very-long-thr");
#endif
}

TEST_CASE("A stop request reaches the body's stop_token", "[thread]") {
  std::atomic<bool> stopped{false};
  auto thread =
      StartThread({.role = ThreadRole::kWatchdog, .name = "se-watchdog"},
                  [&](const std::stop_token& stop) {
                    while (!stop.stop_requested()) std::this_thread::yield();
                    stopped.store(true);
                  });
  REQUIRE(thread.has_value());
  REQUIRE(thread->Joinable());
  thread->RequestStop();
  CHECK(thread->StopToken().stop_requested());
  thread->Join();
  CHECK(stopped.load());
  CHECK_FALSE(thread->Joinable());
}

TEST_CASE("The destructor of a Thread requests a stop and joins", "[thread]") {
  std::atomic<bool> stopped{false};
  {
    auto thread = StartThread({.role = ThreadRole::kLogWorker, .name = "lw"},
                              [&](const std::stop_token& stop) {
                                while (!stop.stop_requested()) {
                                  std::this_thread::yield();
                                }
                                stopped.store(true);
                              });
    REQUIRE(thread.has_value());
  }
  CHECK(stopped.load());
}

TEST_CASE("StartThread refuses the main role", "[thread]") {
  const auto thread =
      StartThread({.role = ThreadRole::kMain, .name = "main"}, [] {});
  REQUIRE_FALSE(thread.has_value());
  CHECK(thread.error().code == ErrorCode::kInvalidArgument);
}

TEST_CASE("The main thread's RoleScope takes slot 0 and restores the thread",
          "[thread]") {
  const testing::MainThreadScope main;
  const std::string before = CurrentOsThreadName().value_or("");
  const ThreadRole role_before = CurrentRole();
  {
    const RoleScope scope({.role = ThreadRole::kMain, .name = "main"});
    REQUIRE(scope.Slot() == std::optional<std::uint32_t>(kMainThreadSlot));
    CHECK(CurrentRole() == ThreadRole::kMain);
    REQUIRE(CurrentOsThreadName().value_or("?") == "main");
    CHECK(CurrentThreadLogRing() ==
          ThreadRegistry::Instance().SlotLogRing(kMainThreadSlot));
    CHECK(scope.OsNameApplied());
  }
  CHECK(CurrentRole() == role_before);
  CHECK(CurrentOsThreadName().value_or("?") == before);
  CHECK(CurrentThreadLogRing() == nullptr);
  CHECK(ThreadBreadcrumb().BoundWord() == nullptr);
}

TEST_CASE("QoS and CPU-set requests succeed or fail softly", "[thread][qos]") {
  const auto topology = ReadCpuTopology();
  REQUIRE(topology.has_value());
  for (const Qos qos : {Qos::kDefault, Qos::kHigh, Qos::kEco}) {
    bool qos_applied = false;
    bool cpus_applied = false;
    auto thread = StartThread(
        {.role = ThreadRole::kForkWorker,
         .name = "se-qos",
         .qos = qos,
         .cpus = WorkerCpuSet(*topology, jobs::CoreClass::kAnyPhysical)},
        [&] {
          // Read through a scope of the same thread:
          // a nested RoleScope reports its own calls.
          const RoleScope nested({.role = ThreadRole::kForkWorker,
                                  .name = "se-qos",
                                  .qos = qos,
                                  .cpus = MainCpuSet(*topology)});
          qos_applied = nested.QosApplied();
          cpus_applied = nested.CpuSetApplied();
        });
    REQUIRE(thread.has_value());
    thread->Join();
#if defined(_WIN32)
    // Windows 10 and later support both calls for the calling thread.
    CHECK(qos_applied);
    CHECK(cpus_applied);
#else
    static_cast<void>(qos_applied);
    static_cast<void>(cpus_applied);
#endif
  }
  // An impossible CPU set is refused softly; the thread still runs.
  bool ran = false;
  bool applied = true;
  auto thread = StartThread(
      {.role = ThreadRole::kBackground, .name = "se-bad-cpus"}, [&] {
        const RoleScope bad({.role = ThreadRole::kBackground,
                             .name = "se-bad-cpus",
                             .cpus = {.os_ids = {0x7FFFFFF0U}}});
        applied = bad.CpuSetApplied();
        ran = true;
      });
  REQUIRE(thread.has_value());
  thread->Join();
  CHECK(ran);
  CHECK_FALSE(applied);
}

}  // namespace
}  // namespace se
