// Copyright 2026 The simple-engine-2 Authors.
//
// game_rampage's fork sites (docs/design/mt-design.md "Job System",
// candidate sites; docs/milestone-specs.md M14). Each system asks its pool
// for the site's record (ForkJoinPool::Site), and every site is disabled in
// every shipped configuration: the enabling rules are M16's, and tests
// enable them explicitly. Disabled, a site runs the same fixed chunks
// inline, so the results do not depend on W.
//
//   rampage.KinematicMove  the player's integration (KinematicBody ->
//                          Transform); Moved per chunk
//   rampage.Path           path following (R6, R12, R13); Moved per chunk
//   rampage.Boundary       the arena clamp (R11)
//   rampage.Broadphase     the CollisionSystem's boxes, source flags and
//                          spatial-hash cells of each candidate, per chunk;
//                          the cells are sorted on the owner after the join
//   rampage.Narrowphase    each source's candidate pairs and overlap tests;
//                          contacts per chunk, concatenated in chunk order,
//                          then sorted (R8)
// Besides these, the Schedule's built-in "se.PrevTransformCopy"
// (se_core/schedule.h) and the RenderList build's "render.RenderList"
// (se_render_core/render_list.h) are fork sites.

#ifndef GAME_RAMPAGE_FORK_SITES_H_
#define GAME_RAMPAGE_FORK_SITES_H_

#include <span>
#include <string_view>

#include "se_core/jobs.h"

namespace se::rampage {

[[nodiscard]] const jobs::ForkSiteDecl& KinematicMoveSite();
[[nodiscard]] const jobs::ForkSiteDecl& PathSite();
[[nodiscard]] const jobs::ForkSiteDecl& BoundarySite();
[[nodiscard]] const jobs::ForkSiteDecl& BroadphaseSite();
[[nodiscard]] const jobs::ForkSiteDecl& NarrowphaseSite();

// The names of the sites above, in that order.
[[nodiscard]] std::span<const std::string_view> ForkSiteNames() noexcept;

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_FORK_SITES_H_
