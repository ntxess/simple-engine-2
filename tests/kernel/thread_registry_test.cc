// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/thread_registry.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "se_core/breadcrumb.h"
#include "se_core/dump_stream.h"
#include "se_core/log_ring.h"
#include "se_core/thread_role.h"

namespace se {
namespace {

// Runs |body| on a new thread that holds a ThreadSlotScope.
template <class F>
void OnRegisteredThread(ThreadRole role, std::string_view name,
                        std::uint64_t os_id, const F& body) {
  std::thread thread([&] {
    const ThreadSlotScope scope(role, name, os_id);
    body(scope.Slot());
  });
  thread.join();
}

TEST_CASE("A released slot is reused, and its ring is never freed",
          "[thread_registry]") {
  ThreadRegistry& registry = ThreadRegistry::Instance();
  std::optional<std::uint32_t> first;
  LogRing* first_ring = nullptr;
  OnRegisteredThread(
      ThreadRole::kBackground, "first", 101,
      [&](std::optional<std::uint32_t> slot) {
        REQUIRE(slot.has_value());
        first = slot;
        first_ring = CurrentThreadLogRing();
        REQUIRE(first_ring != nullptr);
        REQUIRE(registry.Read(*slot).state == ThreadSlotState::kLive);
      });
  REQUIRE(first.has_value());
  CHECK(*first != kMainThreadSlot);
  REQUIRE(registry.Read(*first).state == ThreadSlotState::kExited);
  // The ring outlives the thread: a dump can still read it.
  REQUIRE(registry.SlotLogRing(*first) == first_ring);
  REQUIRE(std::string_view(first_ring->Name().data()) == "first");

  std::optional<std::uint32_t> second;
  OnRegisteredThread(ThreadRole::kWatchdog, "second", 202,
                     [&](std::optional<std::uint32_t> slot) {
                       second = slot;
                       REQUIRE(CurrentThreadLogRing() == first_ring);
                       REQUIRE(CurrentOsThreadId() == 202);
                     });
  REQUIRE(second == first);
  REQUIRE(registry.SlotLogRing(*second) == first_ring);
  const ThreadSlotInfo info = registry.Read(*second);
  REQUIRE(info.Name() == "second");
  CHECK(info.role == ThreadRole::kWatchdog);
  REQUIRE(info.os_thread_id == 202);
  REQUIRE(std::string_view(first_ring->Name().data()) == "second");
  CHECK(first_ring->Role() == ThreadRole::kWatchdog);
  CHECK(first_ring->Capacity() == kThreadLogRingCapacity);
}

TEST_CASE("The main thread takes slot 0 and the largest ring",
          "[thread_registry]") {
  ThreadRegistry& registry = ThreadRegistry::Instance();
  OnRegisteredThread(
      ThreadRole::kMain, "main", 7, [&](std::optional<std::uint32_t> slot) {
        REQUIRE(slot == std::optional<std::uint32_t>(0));
        REQUIRE(CurrentThreadSlot() == slot);
        REQUIRE(CurrentThreadLogRing()->Capacity() == kMainLogRingCapacity);
        // A second main scope cannot take slot 0 twice.
        const ThreadSlotScope nested(ThreadRole::kMain, "main-2", 7);
        REQUIRE(nested.Slot().has_value());
        CHECK(*nested.Slot() != kMainThreadSlot);
      });
  REQUIRE(registry.Read(kMainThreadSlot).Name() == "main");
}

TEST_CASE("Names are cut to 16 characters", "[thread_registry]") {
  ThreadRegistry& registry = ThreadRegistry::Instance();
  OnRegisteredThread(
      ThreadRole::kBackground, "abcdefghijklmnopqrstuvwxyz", 9,
      [&](std::optional<std::uint32_t> slot) {
        REQUIRE(slot.has_value());
        REQUIRE(registry.Read(*slot).Name() == "abcdefghijklmnop");
      });
}

TEST_CASE("The breadcrumb of a registered thread is its slot's word",
          "[thread_registry][breadcrumb]") {
  ThreadRegistry& registry = ThreadRegistry::Instance();
  OnRegisteredThread(ThreadRole::kBackground, "crumbs", 11,
                     [&](std::optional<std::uint32_t> slot) {
                       REQUIRE(slot.has_value());
                       ThreadBreadcrumb().SetTick(4321);
                       ThreadBreadcrumb().SetPhase(3);
                       const BreadcrumbFields fields =
                           UnpackBreadcrumb(registry.Read(*slot).breadcrumb);
                       REQUIRE(fields.tick == 4321);
                       REQUIRE(fields.phase == 3);
                       CHECK(fields.role == static_cast<std::uint8_t>(
                                                ThreadRole::kBackground));
                       REQUIRE(ThreadBreadcrumb().BoundWord() ==
                               registry.BreadcrumbWord(*slot));
                     });
  // Without a scope the thread keeps its own word.
  std::thread plain([] {
    CHECK(ThreadBreadcrumb().BoundWord() == nullptr);
    CHECK(CurrentThreadLogRing() == nullptr);
    CHECK_FALSE(CurrentThreadSlot().has_value());
  });
  plain.join();
}

TEST_CASE("Nested scopes restore the outer binding", "[thread_registry]") {
  std::thread thread([] {
    ThreadBreadcrumb().SetTick(5);
    {
      const ThreadSlotScope outer(ThreadRole::kBackground, "outer", 1);
      LogRing* outer_ring = CurrentThreadLogRing();
      std::uint64_t* outer_word = ThreadBreadcrumb().BoundWord();
      REQUIRE(ThreadBreadcrumb().Fields().tick == 5);
      {
        const ThreadSlotScope inner(ThreadRole::kBackground, "inner", 1);
        CHECK(CurrentThreadLogRing() != outer_ring);
        ThreadBreadcrumb().SetTick(6);
      }
      CHECK(CurrentThreadLogRing() == outer_ring);
      CHECK(ThreadBreadcrumb().BoundWord() == outer_word);
      REQUIRE(ThreadBreadcrumb().Fields().tick == 6);
    }
    CHECK(ThreadBreadcrumb().BoundWord() == nullptr);
    REQUIRE(ThreadBreadcrumb().Fields().tick == 6);
  });
  thread.join();
}

TEST_CASE("A full table refuses the next thread and counts it",
          "[thread_registry]") {
  ThreadRegistry& registry = ThreadRegistry::Instance();
  const std::uint64_t refused = registry.Refused();
  std::vector<std::uint32_t> taken;
  for (;;) {
    const std::optional<std::uint32_t> slot =
        registry.Acquire(ThreadRole::kBackground, "filler", 1);
    if (!slot) break;
    taken.push_back(*slot);
  }
  CHECK(taken.size() <= kThreadSlotCount - 1);
  REQUIRE(registry.Refused() == refused + 1);
  for (const std::uint32_t slot : taken) registry.Release(slot);
  const std::optional<std::uint32_t> again =
      registry.Acquire(ThreadRole::kBackground, "again", 1);
  REQUIRE(again.has_value());
  registry.Release(*again);
}

TEST_CASE("The breadcrumb table is one kind-1 dump stream",
          "[thread_registry][dump_stream]") {
  ThreadRegistry& registry = ThreadRegistry::Instance();
  OnRegisteredThread(ThreadRole::kBackground, "streamed", 3,
                     [](std::optional<std::uint32_t>) {});
  const std::span<const std::byte> stream = registry.BreadcrumbStream();
  REQUIRE(stream.size() == sizeof(DumpStreamHeader) +
                               kThreadSlotCount * sizeof(BreadcrumbRecord));
  DumpStreamHeader header;
  std::memcpy(&header, stream.data(), sizeof(header));
  CHECK(header.magic == kDumpStreamMagic);
  REQUIRE(header.version == 1);
  CHECK(header.kind ==
        static_cast<std::uint16_t>(DumpStreamKind::kBreadcrumbs));
  CHECK(header.record_size == sizeof(BreadcrumbRecord));
  CHECK(header.capacity == kThreadSlotCount);
  CHECK(header.head == registry.SlotsUsed());
  REQUIRE(header.head >= 2);
  REQUIRE(std::string_view(header.name) == "breadcrumbs");
}

struct HookLog {
  std::vector<std::uint32_t> log_rings;
  int tables = 0;
};

void RecordRegion(void* context, DumpStreamKind kind, std::uint32_t index,
                  std::span<const std::byte> region) {
  auto& log = *static_cast<HookLog*>(context);
  if (kind == DumpStreamKind::kBreadcrumbs) {
    ++log.tables;
    CHECK(region.size() == std::size_t{65} * 64);
  } else {
    log.log_rings.push_back(index);
    DumpStreamHeader header;
    std::memcpy(&header, region.data(), sizeof(header));
    CHECK(region.size() ==
          sizeof(DumpStreamHeader) + header.capacity * sizeof(LogRecord));
  }
}

TEST_CASE("The stream hook sees every region, now and later",
          "[thread_registry][dump_stream]") {
  ThreadRegistry& registry = ThreadRegistry::Instance();
  HookLog log;
  registry.SetStreamHook(&RecordRegion, &log);
  REQUIRE(log.tables == 1);
  REQUIRE_FALSE(log.log_rings.empty());
  CHECK(std::find(log.log_rings.begin(), log.log_rings.end(),
                  kLibraryLogRingIndex) != log.log_rings.end());
  // A slot's first use allocates its ring, which reaches the hook. (Every
  // slot has a ring already when the whole suite runs in one process.)
  bool unused_slot = false;
  for (std::uint32_t slot = 1; slot < kThreadSlotCount; ++slot) {
    unused_slot = unused_slot || registry.SlotLogRing(slot) == nullptr;
  }
  const std::size_t before = log.log_rings.size();
  std::vector<std::uint32_t> taken;
  while (unused_slot && log.log_rings.size() == before) {
    const auto slot = registry.Acquire(ThreadRole::kBackground, "hooked", 1);
    if (!slot) break;
    taken.push_back(*slot);
  }
  registry.SetStreamHook(nullptr, nullptr);
  for (const std::uint32_t slot : taken) registry.Release(slot);
  if (unused_slot) CHECK(log.log_rings.size() == before + 1);
}

}  // namespace
}  // namespace se
