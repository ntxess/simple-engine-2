#include "CollisionSystem.hpp"

#include "util/Logger.hpp"

CollisionSystem::CollisionSystem(entt::registry& piplineReg, const sf::Vector2f& center, const sf::Vector2u& size)
	: m_piplineReg{piplineReg}
    , m_quadTree{std::make_unique<QuadTree>(sf::Rect<float>{{center.x, center.y}, {static_cast<float>(size.x), static_cast<float>(size.y)}})}
{
    //const auto& view = reg.view<Sprite>();
    //for (const auto& entityID : view)
    //{
    //    m_quadTree->insert(reg, entityID);
    //}
}


void CollisionSystem::update(entt::registry& entityReg, const float& dt)
{
    LOG_TRACE(Logger::get()) << "Entering CollisionSystem::update()";

    quadTreeUpdate(entityReg);  // Rebuild the quadtree for querying collisions
    collisionUpdate(entityReg); // Find and mark all collided entity with a tag

    LOG_TRACE(Logger::get()) << "Leaving CollisionSystem::update()";
}

void CollisionSystem::quadTreeUpdate(entt::registry& entityReg)
{
    const auto& eventView = entityReg.view<Sprite, UpdateEntityEvent>();
    for (const auto& entityID : eventView)
    {
        m_quadTree->remove(entityReg, entityID);
        m_quadTree->insert(entityReg, entityID);
        LOG_TRACE(Logger::get()) << "Updating event-driven entity [" << static_cast<unsigned int>(entityID) << "] in quadtree";
    }

    const auto& pollingView = entityReg.view<Sprite, UpdateEntityPolling>();
    for (const auto& entityID : pollingView)
    {
        m_quadTree->remove(entityReg, entityID);
        m_quadTree->insert(entityReg, entityID);
        LOG_TRACE(Logger::get()) << "Updating polling entity [" << static_cast<unsigned int>(entityID) << "] in quadtree";
    }
}

void CollisionSystem::collisionUpdate(entt::registry& entityReg)
{
    // Event-driven collision update
    const auto& eventView = entityReg.view<Sprite, TeamTag, EffectsList, UpdateEntityEvent>();
    for (const auto& sourceID : eventView)
    {
        collisionUpdateImpl(entityReg, sourceID);

        // QuadTree update completed, delete dirty flags
        entityReg.remove<UpdateEntityEvent>(sourceID);
    }

    // Polling collision update
    const auto& pollingView = entityReg.view<Sprite, TeamTag, EffectsList, UpdateEntityPolling>();
    for (const auto& sourceID : pollingView)
    {
        collisionUpdateImpl(entityReg, sourceID);
    }
}

void CollisionSystem::collisionUpdateImpl(entt::registry& entityReg, const entt::entity sourceID)
{
    // Query all neighboring entity for collision
    const sf::FloatRect& sourceHitbox = entityReg.get<Sprite>(sourceID).getGlobalBounds();
    std::vector<entt::entity> receiverList = m_quadTree->queryRange(entityReg, sourceHitbox);

    for (const auto& receiverID : receiverList)
    {
        if (sourceID == receiverID)
        {
            LOG_TRACE(Logger::get()) << "Entity [" << static_cast<unsigned int>(sourceID) << "] collided with self";
            continue;
        }
        else if (entityReg.get<TeamTag>(sourceID).tag == entityReg.get<TeamTag>(receiverID).tag)
        {
            LOG_TRACE(Logger::get()) << "Entity [" << static_cast<unsigned int>(sourceID) << "] collided with same team";
            continue;
        }
        else if (entityReg.all_of<UpdateEntityPolling>(sourceID) && !entityReg.get<UpdateEntityPolling>(sourceID).isReady())
        {
            LOG_TRACE(Logger::get()) << "Entity [" << static_cast<unsigned int>(sourceID) << "] is not ready to collide";
            continue;
        }

        // For all of the source entity modifiers, apply effects to receiver
        if (entityReg.all_of<EffectsList>(sourceID))
        {
            for (auto& [effectType, effect] : entityReg.get<EffectsList>(sourceID).effectsList)
            {
                // Get the receiver status and apply effects
                if (entityReg.all_of<EntityStatus>(receiverID))
                {
                    entt::entity statusModEventID = m_piplineReg.create();

                    m_piplineReg.emplace_or_replace<StatusModEvent>(statusModEventID, sourceID, receiverID, effectType, &effect);

                    LOG_INFO(Logger::get())
                        << "Collision Event ID [" << static_cast<unsigned int>(statusModEventID)
                        << "]: Entity [" << static_cast<unsigned int>(sourceID)
                        << "] collided with entity [" << static_cast<unsigned int>(receiverID) << "]";
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
