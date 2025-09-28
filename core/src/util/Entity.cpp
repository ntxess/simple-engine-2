#include "Entity.hpp"

Entity::Entity(std::reference_wrapper<entt::registry> reg)
    : m_reg(reg)
{
    m_entityId = m_reg.get().create();
}

Entity::Entity(std::reference_wrapper<entt::registry> reg, const entt::entity entityID)
    : m_reg(reg)
    , m_entityId(entityID)
{}

Entity::Entity(const Entity& other)
    : m_reg(other.m_reg)
{
    m_entityId = m_reg.get().create();

    // Deep copy of components over
    for (auto [id, storage] : m_reg.get().storage())
    {
        if (storage.contains(other.m_entityId))
        {
            storage.push(m_entityId, storage.value(other.m_entityId));
        }
    }
}

Entity& Entity::operator=(const Entity& other)
{
    m_reg = other.m_reg;

    // Deep copy of components over
    for (auto [id, storage] : m_reg.get().storage())
    {
        if (storage.contains(other.m_entityId))
        {
            storage.push(m_entityId, storage.value(other.m_entityId));
        }
    }
    return *this;
}

Entity::~Entity()
{
    // Entity is responsible for its on destruction
    m_reg.get().destroy(m_entityId);
}

const entt::entity Entity::getId() const
{
    return m_entityId;
}