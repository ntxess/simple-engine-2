// Copyright 2026 The simple-engine-2 Authors.

#include "render/render_fixture.h"

#include <cstdint>
#include <entt/entity/registry.hpp>

#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/schedule.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"

namespace se::testing {

const ComponentRegistry& RenderRegistry() {
  static const ComponentRegistry kRegistry = [] {
    ComponentRegistry r;
    RegisterCoreComponents(r);
    RegisterRenderComponents(r);
    r.Seal();
    return r;
  }();
  return kRegistry;
}

Camera2D WorldCamera(float w, float h) {
  Camera2D camera;
  camera.center = {0.5F * w, 0.5F * h};
  camera.zoom = 1.0F;
  camera.virtual_size = {w, h};
  return camera;
}

SpriteRenderer CenteredSprite(std::uint32_t texture, float w, float h) {
  SpriteRenderer sprite;
  sprite.texture = texture;
  sprite.region = {0.0F, 0.0F, w, h};
  sprite.origin = {0.5F * w, 0.5F * h};
  return sprite;
}

entt::entity AddSprite(World& world, std::uint64_t id, Vec2 pos,
                       const SpriteRenderer& sprite, float rot) {
  const entt::entity entity = world.Create(Guid{0, id});
  Transform transform;
  transform.pos = pos;
  transform.rot = rot;
  world.Reg().emplace<Transform>(entity, transform);
  world.Reg().emplace<SpriteRenderer>(entity, sprite);
  world.FinalizeSpawns();
  return entity;
}

void StepTick(World& world) {
  static Schedule schedule = [] {
    Schedule s;
    s.Seal();
    return s;
  }();
  jobs::ForkJoinPool pool;
  const InputFrame input{};
  schedule.Step(world, input, pool);
}

Transform& MutableTransform(World& world, entt::entity entity) {
  return world.Reg().get<Transform>(entity);
}

}  // namespace se::testing
