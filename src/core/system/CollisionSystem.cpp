#include "CollisionSystem.hpp"

CollisionSystem::CollisionSystem(entt::registry& reg, const sf::Vector2f& center, const sf::Vector2u& size)
    : m_quadTree(std::make_unique<QuadTree>(sf::FloatRect(center.x, center.y, static_cast<float>(size.x), static_cast<float>(size.y))))
{
    const auto& view = reg.view<Sprite>();
    for (const auto& entityID : view)
    {
        m_quadTree->insert(reg, entityID);
    }
}

constexpr std::string_view CollisionSystem::name() const
{
    return "CollisionSystem";
}

void CollisionSystem::update(entt::registry& reg, const float& dt)
{
    LOG_TRACE(Logger::get()) << "Entering CollisionSystem::update()";

    quadTreeUpdate(reg);  // Rebuild the quadtree for querying collisions
    collisionUpdate(reg); // Find and mark all collided entity with a tag

    LOG_TRACE(Logger::get()) << "Leaving CollisionSystem::update()";
}

void CollisionSystem::quadTreeUpdate(entt::registry& reg)
{
    const auto& eventView = reg.view<Sprite, UpdateEntityEvent>();
    for (const auto& entityID : eventView)
    {
        m_quadTree->remove(reg, entityID);
        m_quadTree->insert(reg, entityID);
        LOG_TRACE(Logger::get()) << "Updating event-driven entity [" << static_cast<unsigned int>(entityID) << "] in quadtree";
    }

    const auto& pollingView = reg.view<Sprite, UpdateEntityPolling>();
    for (const auto& entityID : pollingView)
    {
        m_quadTree->remove(reg, entityID);
        m_quadTree->insert(reg, entityID);
        LOG_TRACE(Logger::get()) << "Updating polling entity [" << static_cast<unsigned int>(entityID) << "] in quadtree";
    }
}

void CollisionSystem::collisionUpdate(entt::registry& reg)
{
    // Event-driven collision update
    const auto& eventView = reg.view<Sprite, TeamTag, EffectsList, UpdateEntityEvent>();
    for (const auto& sourceID : eventView)
    {
        // Query all neighboring entity for collision
        const sf::FloatRect& sourceHitbox = reg.get<Sprite>(sourceID).getGlobalBounds();
        std::vector<entt::entity> receiverList = m_quadTree->queryRange(reg, sourceHitbox);

        for (const auto& receiverID : receiverList)
        {
            if (sourceID == receiverID)
            {
                LOG_TRACE(Logger::get()) << "Entity [" << static_cast<unsigned int>(sourceID) << "] collided with self";
                continue;
            }
            else if (reg.get<TeamTag>(sourceID).tag == reg.get<TeamTag>(receiverID).tag)
            {
                LOG_TRACE(Logger::get()) << "Entity [" << static_cast<unsigned int>(sourceID) << "] collided with same team";
                continue;
            }
            else if (!reg.valid(sourceID) && !reg.get<UpdateEntityPolling>(sourceID).isReady())
            {
                LOG_TRACE(Logger::get()) << "Entity [" << static_cast<unsigned int>(sourceID) << "] is not ready to collide";
                continue;
            }

            // For all of the source entity modifiers, apply effects to receiver
            if (reg.valid(sourceID) && reg.all_of<EffectsList>(sourceID))
            {
                for (auto& [effectType, effect] : reg.get<EffectsList>(sourceID).effectsList)
                {
                    // Get the receiver status and apply effects
                    if (reg.valid(receiverID) && reg.all_of<EntityStatus>(receiverID))
                    {
                        StatusModEvent statusModEvent(sourceID, receiverID, effectType, &effect);
                        entt::entity statusModEventID = reg.create();

                        reg.emplace_or_replace<StatusModEvent>(statusModEventID, statusModEvent);

                        LOG_INFO(Logger::get())
                            << "Collision Event ID [" << static_cast<unsigned int>(statusModEventID)
                            << "]: Entity " << static_cast<unsigned int>(sourceID)
                            << " collided with entity " << static_cast<unsigned int>(receiverID);
                    }
                }
            }
        }

        // QuadTree update completed, delete dirty flags
        reg.remove<UpdateEntityEvent>(sourceID);
    }

    // Polling collision update
    const auto& pollingView = reg.view<Sprite, TeamTag, EffectsList, UpdateEntityPolling>();
    for (const auto& sourceID : pollingView)
    {
        // Query all neighboring entity for collision
        const sf::FloatRect& sourceHitbox = reg.get<Sprite>(sourceID).getGlobalBounds();

        if (!reg.valid(sourceID)) continue;

        std::vector<entt::entity> receiverList = m_quadTree->queryRange(reg, sourceHitbox);

        for (const auto& receiverID : receiverList)
        {
            if (sourceID == receiverID)
            {
                LOG_TRACE(Logger::get()) << "Entity [" << static_cast<unsigned int>(sourceID) << "] collided with self";
                continue;
            }
            else if (reg.get<TeamTag>(sourceID).tag == reg.get<TeamTag>(receiverID).tag)
            {
                LOG_TRACE(Logger::get()) << "Entity [" << static_cast<unsigned int>(sourceID) << "] collided with same team";
                continue;
            }
            else if (!reg.get<UpdateEntityPolling>(sourceID).isReady())
            {
                LOG_TRACE(Logger::get()) << "Entity [" << static_cast<unsigned int>(sourceID) << "] is not ready to collide";
                continue;
            }

            // For all of the source entity modifiers, apply effects to receiver
            if (reg.valid(sourceID) && reg.all_of<EffectsList>(sourceID))
            {
                for (auto& [effectType, effect] : reg.get<EffectsList>(sourceID).effectsList)
                {
                    // Get the receiver status and apply effects
                    if (reg.valid(receiverID) && reg.all_of<EntityStatus>(receiverID))
                    {
                        StatusModEvent statusModEvent(sourceID, receiverID, effectType, &effect);
                        entt::entity statusModEventID = reg.create();

                        reg.emplace_or_replace<StatusModEvent>(statusModEventID, statusModEvent);

                        LOG_INFO(Logger::get())
                            << "Collision Event ID [" << static_cast<unsigned int>(statusModEventID)
                            << "]: Entity [" << static_cast<unsigned int>(sourceID)
                            << "] collided with entity [" << static_cast<unsigned int>(receiverID) << "]";
                    }
                }
            }
        }
    }
}

void CollisionSystem::remove(entt::registry& reg, const entt::entity entityID)
{
    m_quadTree->remove(reg, entityID);
    LOG_INFO(Logger::get()) << "Removed entity [" << static_cast<unsigned int>(entityID) << "] from Quadtree";
}

void CollisionSystem::clear()
{
    m_quadTree->clear();
    LOG_INFO(Logger::get()) << "Cleared all entities from Quadtree";
}

void CollisionSystem::draw(sf::RenderTexture& rt)
{
    m_quadTree->draw(rt);
}
