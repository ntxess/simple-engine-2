// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor_panels/overlays.h"

#include <entt/entity/registry.hpp>
#include <format>
#include <optional>
#include <string>

#include "game_rampage/broadphase.h"
#include "game_rampage/components.h"
#include "se_core/core_components.h"
#include "se_core/detmath.h"
#include "se_core/guid.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_editor_panels/picking.h"
#include "se_editor_panels/viewport_math.h"
#include "se_render_core/render_components.h"

namespace se::editor {
namespace {

constexpr float kQuarterTurn = 1.57079632679489661923F;

// The box the legacy "Entity Box Visualizer" drew: the collider when there
// is one, else the sprite's bounds.
rampage::Aabb BoxOf(const entt::registry& reg, entt::entity e,
                    const Transform& transform, const SpriteRenderer& sprite) {
  if (const auto* collider = reg.try_get<rampage::Collider>(e)) {
    return rampage::ColliderBox(transform.pos, collider->half_extents);
  }
  const PickBounds bounds = SpriteBounds(transform, sprite);
  return rampage::Aabb{bounds.min, bounds.max};
}

OverlayQuad ScreenQuad(const rampage::Aabb& box, const ViewportMapping& map,
                       Rgba8 color) {
  return OverlayQuad{
      {map.WorldToScreen(box.min), map.WorldToScreen({box.max.x, box.min.y}),
       map.WorldToScreen(box.max), map.WorldToScreen({box.min.x, box.max.y})},
      color};
}

Vec2 Heading(float angle) {
  const dm::SinCosPair<float> sc = dm::SinCos(angle);
  return {sc.cos * kHeadingLength, sc.sin * kHeadingLength};
}

}  // namespace

void BuildOverlays(const World& world, const OverlayOptions& options,
                   const ViewportMapping& mapping, Overlays& out) {
  out.Clear();
  if (!options.Any()) return;
  const entt::registry& reg = world.CReg();

  struct Player {
    entt::entity entity = entt::null;
    rampage::Aabb box;
  };
  std::optional<Player> player;
  if (options.boxes) {
    for (const auto [e, control, transform, sprite] :
         reg.view<const rampage::PlayerControl, const Transform,
                  const SpriteRenderer>()
             .each()) {
      player = Player{e, BoxOf(reg, e, transform, sprite)};
      break;
    }
  }

  for (const auto [e, transform, sprite] :
       reg.view<const Transform, const SpriteRenderer>().each()) {
    const PickBounds bounds = SpriteBounds(transform, sprite);
    if (options.ids) {
      const auto* id = reg.try_get<IdComponent>(e);
      const std::string guid = id != nullptr ? ToString(id->guid) : "-";
      out.texts.push_back(
          {mapping.WorldToScreen(bounds.max), guid.substr(0, 8)});
    }
    if (options.boxes) {
      const rampage::Aabb box = BoxOf(reg, e, transform, sprite);
      const bool hit = player.has_value() && player->entity != e &&
                       rampage::Overlaps(player->box, box);
      out.quads.push_back(
          ScreenQuad(box, mapping, hit ? kOverlayRed : kOverlayGreen));
    }
    if (options.headings) {
      const Vec2 from = mapping.WorldToScreen(transform.pos);
      out.lines.push_back(
          {from, mapping.WorldToScreen(transform.pos + Heading(transform.rot)),
           kOverlayWhite});
      out.lines.push_back(
          {from,
           mapping.WorldToScreen(transform.pos +
                                 Heading(transform.rot - kQuarterTurn)),
           kOverlayWhite});
    }
    if (options.positions) {
      out.texts.push_back(
          {mapping.WorldToScreen({bounds.max.x, transform.pos.y}),
           std::format("{:.2f}, {:.2f}", transform.pos.x, transform.pos.y)});
    }
  }
}

}  // namespace se::editor
