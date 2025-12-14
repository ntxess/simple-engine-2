#pragma once 

#include <unordered_map>

#include <SFML/Window/Keyboard.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "interface/ICommand.hpp"
#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"

class PlayerInput : public IComponent
{
public:
    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    void processInput(entt::registry& reg);

    std::unordered_map<sf::Keyboard::Scancode, ICommand*> input;
};