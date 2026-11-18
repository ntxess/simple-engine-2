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

// One action of the game: ActionId i is the i-th binding (bit i of
// InputFrame::held). se_runtime's ActionMap resolves the names (M8).
struct ActionBinding {
  std::string_view action;
  // The default key: an SDL key name ("W", "Space", "Escape") or a legacy
  // config name ("w", "space", "esc", "enter"); "mouse-left" and the other
  // mouse-* names bind a mouse button.
  std::string_view key;
  // The user setting that rebinds the key: control-settings/key-bindings/
  // <config_key> in the game's settings file. Empty: not rebindable.
  std::string_view config_key = {};
  // Default gamepad inputs, comma-separated SDL gamepad names: buttons
  // ("south", "dpup", "start") and half axes ("-lefty", "+leftx").
  std::string_view pad = {};
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
  // The state a player's session starts in (rampage: "title"); the
  // runtime and the editor's 'Play game' start here. start_state stays
  // the one-World state editor Play, se_replay and the goldens use.
  std::string_view game_start_state;
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
