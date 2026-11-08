// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/core_components.h"

#include "se_core/component_registry.h"
#include "se_core/reflect_glaze.h"

namespace se {

void RegisterCoreComponents(ComponentRegistry& registry) {
  using enum ComponentFlags;
  // The Guid is already part of every entity digest, so IdComponent is not
  // hashed again.
  registry.Add<IdComponent>("IdComponent", 1, kSerialized | kEditorVisible);
  registry.Add<Name>("Name", 1, kHashed | kSerialized | kEditorVisible);
  registry.Add<Transform>(
      "Transform", 1, kHashed | kSerialized | kInterpolated | kEditorVisible);
  registry.Add<PrevTransform>("PrevTransform", 1, kHashed);
  registry.Add<Spawned>("Spawned", 1, kHashed | kSerialized);
  registry.Add<DestroyRequest>("DestroyRequest", 1, kNone);
}

}  // namespace se
