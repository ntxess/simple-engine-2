#pragma once 

#include "interface/ICommand.hpp"
#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"
#include "entt/entity/entity.hpp"
#include "entt/entity/registry.hpp"
#include <SFML/Window/Keyboard.hpp>
#include <unordered_map>

class PlayerInput : public IComponent
{
public:
    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    void processInput(entt::registry& reg);

    std::unordered_map<sf::Keyboard::Key, ICommand*> input;
};