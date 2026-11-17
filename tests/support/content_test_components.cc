// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/component_registry.h"
#include "se_core/reflect_glaze.h"
#include "support/content_test_module.h"

namespace se::testing {

void RegisterContentTestComponents(ComponentRegistry& registry) {
  using enum ComponentFlags;
  registry.Add<Drift>("Drift", 1, kHashed | kSerialized);
  registry.Add<ContentStamp>("ContentStamp", 1, kHashed | kSerialized);
}

}  // namespace se::testing
