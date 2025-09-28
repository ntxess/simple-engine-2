#include "Hitbox.hpp"

Hitbox::Hitbox(const sf::VertexArray& verticies)
    : hitbox(verticies)
{}

Hitbox::Hitbox(const sf::PrimitiveType type, const std::size_t vertexCount)
    : hitbox(type, vertexCount)
{}

void Hitbox::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}