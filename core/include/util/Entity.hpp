#pragma once

#include "entt/entity/entity.hpp"
#include "entt/entity/registry.hpp"

class Entity
{
public:
    Entity(std::reference_wrapper<entt::registry> reg);
    Entity(std::reference_wrapper<entt::registry> reg, const entt::entity entityID);
    Entity(const Entity& other);
    Entity& operator=(const Entity& other);
    ~Entity();
    operator bool() const { return m_entityId != entt::null; }

    const entt::entity getId() const;

    template<typename T, typename... Args>
    T& addComponent(Args&&... args);

    template<typename T>
    void addComponent();

    template<typename T>
    T& getComponent() const;

    template<typename T>
    bool hasComponent() const;

    template<typename T>
    void removeComponent();

private:
    entt::entity m_entityId;
    std::reference_wrapper<entt::registry> m_reg;
};

template<typename T, typename... Args>
inline T& Entity::addComponent(Args&&... args)
{
    return m_reg.get().emplace<T>(m_entityId, std::forward<Args>(args)...);
}

template<typename T>
inline void Entity::addComponent()
{
    m_reg.get().emplace<T>(m_entityId);
}

template<typename T>
inline T& Entity::getComponent() const
{
    return m_reg.get().get<T>(m_entityId);
}

template<typename T>
inline bool Entity::hasComponent() const
{
    return m_reg.get().all_of<T>(m_entityId);
}

template<typename T>
inline void Entity::removeComponent()
{
    m_reg.get().remove<T>(m_entityId);
}