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
    CollisionSystem(entt::registry& piplineReg, const sf::Vector2f& center, const sf::Vector2u& size);

    constexpr std::string_view name() const;
    void update(entt::registry& entityReg, const float& dt = 0.f);
    void remove(entt::registry& entityReg, const entt::entity entityID);
    void clear();
    void draw(sf::RenderTexture& rt);

private:
    void quadTreeUpdate(entt::registry& entityReg);
    void collisionUpdate(entt::registry& entityReg);
	void collisionUpdateImpl(entt::registry& entityReg, const entt::entity sourceID);

private:
	entt::registry& m_piplineReg;
    std::unique_ptr<QuadTree> m_quadTree;
};