// Copyright 2026 The simple-engine-2 Authors.

#include "support/kernel_test_components.h"

#include "se_core/component_registry.h"
#include "se_core/reflect_glaze.h"

namespace se::testing {

void RegisterKernelTestComponents(ComponentRegistry& registry) {
  using enum ComponentFlags;
  registry.Add<Mover>("Mover", 1, kHashed | kSerialized | kEditorVisible);
  registry.Add<Spawner>("Spawner", 1, kHashed | kSerialized);
  registry.Add<Lifetime>("Lifetime", 1, kHashed | kSerialized);
  registry.Add<Player>("Player", 1, kHashed | kSerialized);
  registry.Add<Follow>("Follow", 1, kHashed | kSerialized);
  registry.Add<Banner>("Banner", 1, kHashed | kSerialized);
}

}  // namespace se::testing
