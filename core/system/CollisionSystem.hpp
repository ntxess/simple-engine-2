#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <SFML/Graphics/RenderTexture.hpp>
#include <SFML/System/Vector2.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "Components.hpp"
#include "interface/ISystem.hpp"
#include "util/LegacySimTime.hpp"
#include "util/QuadTree.hpp"

class CollisionSystem : public ISystem
{
public:
    CollisionSystem() = delete;
    CollisionSystem(entt::registry& piplineReg, const LegacySimTime& simTime, const sf::Vector2f& center, const sf::Vector2u& size);

    constexpr std::string_view name() const override final;
    static sf::FloatRect colliderOf(const Sprite& sprite);
    void update(entt::registry& entityReg, const float& dt = 0.f) override final;
    void remove(entt::registry& entityReg, const entt::entity entityID);
    void clear();
    void draw(sf::RenderTexture& rt);

private:
    void quadTreeUpdate(entt::registry& entityReg);
    void collisionUpdate(entt::registry& entityReg);
    void collectContacts(entt::registry& entityReg, const entt::entity sourceID);
    void applyContact(entt::registry& entityReg, const entt::entity sourceID, const entt::entity receiverID);

    // One overlapping (source, receiver) pair of the current tick (rule R8).
    struct Contact
    {
        std::uint32_t sourceIndex;
        std::uint32_t receiverIndex;
        entt::entity sourceID;
        entt::entity receiverID;
    };

private:
	entt::registry& m_piplineReg;
    const LegacySimTime& m_simTime;
    std::unique_ptr<QuadTree> m_quadTree;
    std::vector<std::pair<std::uint32_t, entt::entity>> m_candidates; // Receivers of this tick, by SpawnIndex
    std::vector<Contact> m_contacts;                                  // Contacts of this tick (rule R8)
    std::uint64_t m_nextEventSequence = 0;                            // Creation order of StatusModEvents
};