#pragma once

#include "../Components.hpp"
#include "../interface/ISystem.hpp"
#include "../util/Logger.hpp"
#include "../util/QuadTree.hpp"
#include "entt/entity/entity.hpp"
#include "entt/entity/registry.hpp"
#include "SFML/System/Vector2.hpp"
#include "SFML/Graphics/RenderTexture.hpp"
#include <memory>
#include <string_view>

class CollisionSystem : public ISystem
{
public:
    CollisionSystem() = delete;
    CollisionSystem(entt::registry& reg, const sf::Vector2f& center, const sf::Vector2u& size);

    constexpr std::string_view name() const;
    void update(entt::registry& reg, const float& dt = 0.f);
    void remove(entt::registry& reg, const entt::entity entityID);
    void clear();
    void draw(sf::RenderTexture& rt);

private:
    void quadTreeUpdate(entt::registry& reg);
    void collisionUpdate(entt::registry& reg);

private:
    std::unique_ptr<QuadTree> m_quadTree;
};