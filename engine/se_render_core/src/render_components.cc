// Copyright 2026 The simple-engine-2 Authors.

#include "se_render_core/render_components.h"

#include "se_core/component_registry.h"
#include "se_core/reflect_glaze.h"

namespace se {

void RegisterRenderComponents(ComponentRegistry& registry) {
  using enum ComponentFlags;
  // Serialized, not hashed: rendering state never feeds the simulation.
  registry.Add<SpriteRenderer>("SpriteRenderer", 1,
                               kSerialized | kEditorVisible);
  registry.Add<Camera2D>("Camera2D", 1, kSerialized | kEditorVisible);
}

}  // namespace se
