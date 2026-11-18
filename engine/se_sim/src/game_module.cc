// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/game_module.h"

#include "se_core/assert.h"
#include "se_core/core_components.h"

namespace se {

ModuleInstance::ModuleInstance(const GameModule& module) : module_(&module) {
  RegisterCoreComponents(registry_);
  if (module.register_components != nullptr) {
    module.register_components(registry_);
  }
  registry_.Seal();
  if (module.register_systems != nullptr) module.register_systems(schedule_);
  schedule_.Seal();
  if (module.register_states != nullptr) module.register_states(factory_);
  SE_ASSERT_MSG(
      module.start_state.empty() || factory_.Contains(module.start_state),
      "the module's start state is not registered");
}

}  // namespace se
