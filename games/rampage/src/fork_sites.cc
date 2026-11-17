// Copyright 2026 The simple-engine-2 Authors.

#include "game_rampage/fork_sites.h"

#include <array>
#include <entt/core/type_info.hpp>
#include <span>
#include <string_view>

#include "game_rampage/components.h"
#include "se_core/core_components.h"
#include "se_core/jobs.h"

namespace se::rampage {
namespace {

template <class T>
entt::id_type Id() {
  return entt::type_hash<T>::value();
}

constexpr std::array<std::string_view, 5> kSiteNames = {
    "rampage.KinematicMove", "rampage.Path", "rampage.Boundary",
    "rampage.Broadphase", "rampage.Narrowphase"};

}  // namespace

// Grains from the design's candidate list (1024 items for per-entity work);
// the narrowphase's 256 candidates per chunk are its "32 cells" of a dense
// scene.

const jobs::ForkSiteDecl& KinematicMoveSite() {
  static const std::array<entt::id_type, 1> kReads = {Id<KinematicBody>()};
  static const std::array<entt::id_type, 1> kWrites = {Id<Transform>()};
  static const jobs::ForkSiteDecl kSite{
      .name = kSiteNames[0], .grain = 1024, .reads = kReads, .writes = kWrites};
  return kSite;
}

const jobs::ForkSiteDecl& PathSite() {
  static const std::array<entt::id_type, 1> kReads = {Id<Stats>()};
  static const std::array<entt::id_type, 3> kWrites = {
      Id<Path>(), Id<PathCache>(), Id<Transform>()};
  static const jobs::ForkSiteDecl kSite{
      .name = kSiteNames[1], .grain = 1024, .reads = kReads, .writes = kWrites};
  return kSite;
}

const jobs::ForkSiteDecl& BoundarySite() {
  static const std::array<entt::id_type, 1> kReads = {Id<Collider>()};
  static const std::array<entt::id_type, 1> kWrites = {Id<Transform>()};
  static const jobs::ForkSiteDecl kSite{
      .name = kSiteNames[2], .grain = 1024, .reads = kReads, .writes = kWrites};
  return kSite;
}

const jobs::ForkSiteDecl& BroadphaseSite() {
  static const std::array<entt::id_type, 4> kReads = {
      Id<Transform>(), Id<Collider>(), Id<EffectSource>(), Id<Moved>()};
  static const jobs::ForkSiteDecl kSite{
      .name = kSiteNames[3], .grain = 1024, .reads = kReads};
  return kSite;
}

const jobs::ForkSiteDecl& NarrowphaseSite() {
  // Reads only the CollisionSystem's scratch (boxes, flags, the hash).
  static const jobs::ForkSiteDecl kSite{.name = kSiteNames[4], .grain = 256};
  return kSite;
}

std::span<const std::string_view> ForkSiteNames() noexcept {
  return kSiteNames;
}

}  // namespace se::rampage
