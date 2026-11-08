// Copyright 2026 The simple-engine-2 Authors.
//
// GameModule: a game's statically linked description (not a plugin
// system): its components, systems, states, start state and default action
// bindings. ModuleInstance builds the sealed registry, the sealed Schedule
// and the state factory from one.

#ifndef SE_SIM_GAME_MODULE_H_
#define SE_SIM_GAME_MODULE_H_

#include <memory>
#include <span>
#include <string_view>

#include "se_core/component_registry.h"
#include "se_core/schedule.h"
#include "se_sim/game_state.h"

namespace se {

struct ActionBinding {
  std::string_view action;
  std::string_view key;
};

struct ActionMapDefaults {
  std::span<const ActionBinding> bindings{};
};

struct GameModule {
  std::string_view name;
  void (*register_components)(ComponentRegistry& registry) = nullptr;
  void (*register_systems)(Schedule& schedule) = nullptr;
  void (*register_states)(GameStateFactory& factory) = nullptr;
  std::string_view start_state;
  ActionMapDefaults actions;
};

// The engine side of a loaded module. Not movable: Worlds and stacks keep
// pointers to its members.
class ModuleInstance {
 public:
  // Registers the core components and the module's, then seals the registry
  // and the Schedule.
  explicit ModuleInstance(const GameModule& module);
  ModuleInstance(const ModuleInstance&) = delete;
  ModuleInstance& operator=(const ModuleInstance&) = delete;

  [[nodiscard]] const GameModule& Module() const noexcept { return *module_; }
  [[nodiscard]] const ComponentRegistry& Registry() const noexcept {
    return registry_;
  }
  [[nodiscard]] Schedule& MutableSchedule() noexcept { return schedule_; }
  [[nodiscard]] const GameStateFactory& Factory() const noexcept {
    return factory_;
  }

 private:
  const GameModule* module_;
  ComponentRegistry registry_;
  Schedule schedule_;
  GameStateFactory factory_;
};

}  // namespace se

#endif  // SE_SIM_GAME_MODULE_H_
