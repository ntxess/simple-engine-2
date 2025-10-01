#pragma once

#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"
#include "entt/entity/entity.hpp"
#include <SFML/Graphics/VertexArray.hpp>

class Hitbox : public IComponent, public sf::VertexArray
{
public:
    using sf::VertexArray::VertexArray;
    using sf::VertexArray::getVertexCount;
    using sf::VertexArray::operator[];
    using sf::VertexArray::clear;
    using sf::VertexArray::resize;
    using sf::VertexArray::append;
    using sf::VertexArray::setPrimitiveType;
    using sf::VertexArray::getPrimitiveType;
    using sf::VertexArray::getBounds;

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;
};