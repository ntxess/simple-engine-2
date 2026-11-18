// Copyright 2026 The simple-engine-2 Authors.

#include "game_rampage/view.h"

#include <optional>

#include "game_rampage/components.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"

namespace se::rampage {

Camera2D ArenaCamera(const ArenaBounds& bounds) noexcept {
  Camera2D camera;
  camera.center = {bounds.w * 0.5F, bounds.h * 0.5F};
  camera.zoom = 1.0F;
  camera.virtual_size = {bounds.w, bounds.h};
  camera.primary = true;
  return camera;
}

Camera2D SandboxCamera(const World& world) {
  if (const std::optional<Camera2D> primary = PrimaryCamera(world)) {
    return *primary;
  }
  const auto* bounds = world.CReg().ctx().find<ArenaBounds>();
  return ArenaCamera(bounds != nullptr ? *bounds : ArenaBounds{});
}

}  // namespace se::rampage
