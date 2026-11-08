// Copyright 2026 The simple-engine-2 Authors.
//
// Helpers for the RenderList tests: a registry with the core and render
// components, sprite creation, and a tick step that runs the kernel's own
// Schedule (so PrevTransform is copied exactly as in a game).

#ifndef TESTS_RENDER_RENDER_FIXTURE_H_
#define TESTS_RENDER_RENDER_FIXTURE_H_

#include <cstdint>
#include <entt/entity/fwd.hpp>

#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"

namespace se::testing {

// RegisterCoreComponents and RegisterRenderComponents, sealed.
[[nodiscard]] const ComponentRegistry& RenderRegistry();

// A camera whose canvas is the world rectangle [0, w] x [0, h] at zoom 1, so
// canvas pixels equal world pixels.
[[nodiscard]] Camera2D WorldCamera(float w, float h);

// A w x h sprite on |texture| with its origin at the region's centre.
[[nodiscard]] SpriteRenderer CenteredSprite(std::uint32_t texture, float w,
                                            float h);

// Creates an entity with Guid {0, id}, a Transform at |pos| (rotation in
// degrees) and |sprite|, and finalizes the spawn (PrevTransform = Transform,
// teleport_tick = spawn tick).
entt::entity AddSprite(World& world, std::uint64_t id, Vec2 pos,
                       const SpriteRenderer& sprite, float rot = 0.0F)
    SE_REQUIRES(g_sim_owner);

// Runs one tick of an empty Schedule: the clock advances, PrevTransform takes
// the current Transform, and Cleanup finalizes spawns. Afterwards the test
// sets the tick's new poses, as a Movement system would.
void StepTick(World& world) SE_REQUIRES(g_sim_owner);

// The Transform of |entity|, for the test to move.
[[nodiscard]] Transform& MutableTransform(World& world, entt::entity entity)
    SE_REQUIRES(g_sim_owner);

}  // namespace se::testing

#endif  // TESTS_RENDER_RENDER_FIXTURE_H_
