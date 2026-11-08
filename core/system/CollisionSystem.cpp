#include "CollisionSystem.hpp"

#include <algorithm>

#include "util/Logger.hpp"

CollisionSystem::CollisionSystem(entt::registry& piplineReg, const LegacySimTime& simTime, const sf::Vector2f& center, const sf::Vector2u& size)
	: m_piplineReg{piplineReg}
    , m_simTime{simTime}
    , m_quadTree{std::make_unique<QuadTree>(sf::Rect<float>{{center.x, center.y}, {static_cast<float>(size.x), static_cast<float>(size.y)}})}
{
    //const auto& view = reg.view<Sprite>();
    //for (const auto& entityID : view)
    //{
    //    m_quadTree->insert(reg, entityID);
    //}
}

constexpr std::string_view CollisionSystem::name() const
{
    return "CollisionSystem";
}

sf::FloatRect CollisionSystem::colliderOf(const Sprite& sprite)
{
    // Rule R4: the unrotated texture-size box centred on the sprite position (rotation, scale and
    // origin do not change it).
    const sf::Vector2f size{ static_cast<float>(sprite.getTextureRect().size.x), static_cast<float>(sprite.getTextureRect().size.y) };
    return sf::FloatRect{ sprite.getPosition() - size / 2.f, size };
}

void CollisionSystem::update(entt::registry& entityReg, const float& dt)
{
    LOG_TRACE(Logger::get()) << "Entering CollisionSystem::update()";

    quadTreeUpdate(entityReg);  // Rebuild the quadtree (drawn by the editor's visualizer only)
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
    // Rule R2: cooldowns are kept per (source, receiver) pair; drop the ones that ran out.
    for (auto&& [entityID, polling] : entityReg.view<UpdateEntityPolling>().each())
    {
        polling.forgetExpired(m_simTime.tick);
    }

    // Rule R3: every overlap is found by testing all candidates, in SpawnIndex order, instead of
    // querying the quadtree (which missed entities it never held or held in a node the range missed).
    m_candidates.clear();
    const auto& candidateView = entityReg.view<SpawnIndex, Sprite, TeamTag>();
    for (const auto& entityID : candidateView)
    {
        m_candidates.emplace_back(candidateView.get<SpawnIndex>(entityID).value, entityID);
    }
    std::sort(m_candidates.begin(), m_candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    // Rule R8: collect this tick's contacts, the distinct overlapping (source, receiver) pairs, and
    // apply them sorted by (source, receiver) SpawnIndex.
    m_contacts.clear();

    // Event-driven collision sources
    const auto& eventView = entityReg.view<Sprite, TeamTag, EffectsList, UpdateEntityEvent>();
    for (const auto& sourceID : eventView)
    {
        collectContacts(entityReg, sourceID);

        // Contacts collected, delete dirty flags
        entityReg.remove<UpdateEntityEvent>(sourceID);
    }

    // Polling collision sources
    const auto& pollingView = entityReg.view<Sprite, TeamTag, EffectsList, UpdateEntityPolling>();
    for (const auto& sourceID : pollingView)
    {
        collectContacts(entityReg, sourceID);
    }

    // Entity ids only break ties between sources without a SpawnIndex, so equal pairs end up adjacent.
    std::sort(m_contacts.begin(), m_contacts.end(), [](const Contact& a, const Contact& b) {
        if (a.sourceIndex != b.sourceIndex) return a.sourceIndex < b.sourceIndex;
        if (a.sourceID != b.sourceID) return entt::to_integral(a.sourceID) < entt::to_integral(b.sourceID);
        return a.receiverIndex < b.receiverIndex;
    });
    m_contacts.erase(std::unique(m_contacts.begin(), m_contacts.end(), [](const Contact& a, const Contact& b) {
        return a.sourceID == b.sourceID && a.receiverID == b.receiverID;
    }), m_contacts.end());

    for (const Contact& contact : m_contacts)
    {
        applyContact(entityReg, contact.sourceID, contact.receiverID);
    }
}

void CollisionSystem::collectContacts(entt::registry& entityReg, const entt::entity sourceID)
{
    // Test every candidate for collision (rule R3)
    const sf::FloatRect sourceHitbox = colliderOf(entityReg.get<Sprite>(sourceID));
    const SpawnIndex* sourceIndex = entityReg.try_get<SpawnIndex>(sourceID);

    for (const auto& [receiverIndex, receiverID] : m_candidates)
    {
        if (sourceHitbox.findIntersection(colliderOf(entityReg.get<Sprite>(receiverID))).has_value())
        {
            m_contacts.push_back(Contact{ sourceIndex ? sourceIndex->value : UINT32_MAX, receiverIndex, sourceID, receiverID });
        }
    }
}

void CollisionSystem::applyContact(entt::registry& entityReg, const entt::entity sourceID, const entt::entity receiverID)
{
    if (sourceID == receiverID)
    {
        LOG_TRACE(Logger::get()) << "Entity [" << static_cast<unsigned int>(sourceID) << "] collided with self";
        return;
    }
    else if (entityReg.get<TeamTag>(sourceID).tag == entityReg.get<TeamTag>(receiverID).tag)
    {
        LOG_TRACE(Logger::get()) << "Entity [" << static_cast<unsigned int>(sourceID) << "] collided with same team";
        return;
    }
    else if (entityReg.all_of<UpdateEntityPolling>(sourceID) && !entityReg.get<UpdateEntityPolling>(sourceID).isReadyFor(receiverID, m_simTime.tick))
    {
        LOG_TRACE(Logger::get()) << "Entity [" << static_cast<unsigned int>(sourceID) << "] is not ready to collide";
        return;
    }

    // For all of the source entity modifiers, apply effects to receiver
    if (entityReg.all_of<EffectsList>(sourceID))
    {
        for (const auto& [effectType, effect] : entityReg.get<EffectsList>(sourceID).effectsList)
        {
            // Get the receiver status and apply effects
            if (entityReg.all_of<EntityStatus>(receiverID))
            {
                entt::entity statusModEventID = m_piplineReg.create();

                StatusModEvent& event = m_piplineReg.emplace_or_replace<StatusModEvent>(statusModEventID, sourceID, receiverID, effectType, effect, m_simTime.tick);
                event.sequence = m_nextEventSequence++;

                LOG_INFO(Logger::get())
                    << "Collision Event ID [" << static_cast<unsigned int>(statusModEventID)
                    << "]: Entity [" << static_cast<unsigned int>(sourceID)
                    << "] collided with entity [" << static_cast<unsigned int>(receiverID) << "]";
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
