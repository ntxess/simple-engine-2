// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/schedule.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <string_view>
#include <vector>

#include "se_core/assert.h"
#include "se_core/breadcrumb.h"
#include "se_core/core_components.h"
#include "se_core/profile_clock.h"
#include "se_core/world.h"

namespace se {
namespace {

// The built-in head of Movement: the pose at the end of the previous tick.
void CopyPrevTransforms(World& world) SE_REQUIRES(g_sim_owner) {
  const Tick previous = world.Clock().tick - 1;
  auto view = world.Reg().view<const Transform, PrevTransform>();
  for (const auto [entity, transform, prev] : view.each()) {
    prev.pos = transform.pos;
    prev.rot = transform.rot;
    prev.scale = transform.scale;
    prev.tick = previous;
  }
}

// The built-in end of Cleanup.
void BuiltInCleanup(World& world) SE_REQUIRES(g_sim_owner) {
  world.DrainDestroyRequests();
  world.FinalizeSpawns();
  world.RecordHash();
}

std::uint32_t ClampNs(std::uint64_t ns) {
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(ns, 0xFFFFFFFFU));
}

}  // namespace

std::string_view PhaseName(Phase phase) noexcept {
  switch (phase) {
    case Phase::kInput:
      return "Input";
    case Phase::kIntent:
      return "Intent";
    case Phase::kMovement:
      return "Movement";
    case Phase::kCollision:
      return "Collision";
    case Phase::kEffects:
      return "Effects";
    case Phase::kGameRules:
      return "GameRules";
    case Phase::kCleanup:
      return "Cleanup";
  }
  return "?";
}

void SystemStats::Record(std::uint64_t ns) noexcept {
  const std::uint32_t value = ClampNs(ns);
  ring_ns[head] = value;
  head = static_cast<std::uint32_t>((head + 1) % kSystemRingSize);
  count = std::min<std::uint32_t>(count + 1, kSystemRingSize);
  ++runs;
  last_ns = value;
  max_ns = std::max(max_ns, value);
}

std::uint32_t SystemStats::PercentileNs(double p) const {
  if (count == 0) return 0;
  std::vector<std::uint32_t> values(ring_ns.begin(), ring_ns.begin() + count);
  const auto rank = static_cast<std::size_t>(std::clamp(
      p * static_cast<double>(count), 1.0, static_cast<double>(count)));
  const auto nth = values.begin() + static_cast<std::ptrdiff_t>(rank - 1);
  std::nth_element(values.begin(), nth, values.end());
  return *nth;
}

Schedule::Schedule() {
  stats_.resize(2);
  stats_[kPrevTransformCopyId].name = "PrevTransformCopy";
  stats_[kPrevTransformCopyId].phase = Phase::kMovement;
  stats_[kPrevTransformCopyId].built_in = true;
  stats_[kCleanupId].name = "Cleanup";
  stats_[kCleanupId].phase = Phase::kCleanup;
  stats_[kCleanupId].built_in = true;
}

SystemId Schedule::AddEntry(Phase phase, std::string_view name, RunFn run) {
  SE_ASSERT_MSG(!sealed_, "the Schedule is sealed");
  SE_ASSERT_MSG(stats_.size() < 4096, "breadcrumbs hold 12-bit system ids");
  const auto id = static_cast<SystemId>(stats_.size());
  SystemStats& stats = stats_.emplace_back();
  stats.name = name;
  stats.phase = phase;
  entries_.push_back(Entry{phase, run, id});
  return id;
}

void Schedule::Seal() {
  // Phase order; registration order within a phase.
  std::stable_sort(
      entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        return static_cast<int>(a.phase) < static_cast<int>(b.phase);
      });
  sealed_ = true;
}

void Schedule::Step(World& world, const InputFrame& input,
                    jobs::ForkJoinPool& jobs) {
  SE_ASSERT_MSG(sealed_, "seal the Schedule before stepping");
  const MutationGate gate = world.OpenGate(GateReason::kTick);
  world.MutableClock().Advance();
  const TickContext context{input, world.Clock(), jobs};
  Breadcrumb& crumb = ThreadBreadcrumb();
  crumb.SetTick(world.Clock().tick);

  std::size_t next = 0;
  for (int p = 0; p < kPhaseCount; ++p) {
    const auto phase = static_cast<Phase>(p);
    crumb.SetPhase(static_cast<std::uint8_t>(p));
    if (phase == Phase::kMovement) {
      crumb.SetSystem(kPrevTransformCopyId);
      const std::uint64_t start = ProfileNowNs();
      CopyPrevTransforms(world);
      stats_[kPrevTransformCopyId].Record(ProfileNowNs() - start);
    }
    while (next < entries_.size() && entries_[next].phase == phase) {
      const Entry& entry = entries_[next++];
      crumb.SetSystem(entry.id);
      const std::uint64_t start = ProfileNowNs();
      entry.run(world, context);
      stats_[entry.id].Record(ProfileNowNs() - start);
    }
    if (phase == Phase::kCleanup) {
      crumb.SetSystem(kCleanupId);
      const std::uint64_t start = ProfileNowNs();
      BuiltInCleanup(world);
      stats_[kCleanupId].Record(ProfileNowNs() - start);
    }
  }
  crumb.SetSystem(0);
}

}  // namespace se
