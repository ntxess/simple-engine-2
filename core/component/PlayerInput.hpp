#pragma once 

#include <map>

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "interface/ICommand.hpp"
#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"
#include "util/LegacyInput.hpp"

class PlayerInput : public IComponent
{
public:
    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    // Executes the command of every action held in this tick's input frame, in action order.
    void processInput(entt::registry& reg, const LegacyInputFrame& frame);

    std::map<LegacyAction, ICommand*> input;
};
