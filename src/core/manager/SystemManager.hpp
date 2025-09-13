#pragma once

#include "../Managers.hpp"
#include "../interface/ISystem.hpp"
#include "../util/SystemProfiler.hpp"
#include <deque>
#include <memory>
#include <typeindex>
#include <unordered_map>

class SystemManager
{
public:
    template<typename T, typename... Args>
    void addSystem(bool enableProfiling = false, Args&&... args);

    template<typename T>
    T* getSystem();

    template<typename T>
    void removeSystem();

    void update(entt::registry& reg, const float& dt = 0.f);

    std::unordered_map<std::string, std::deque<long long>> getSystemTimingHistory() const;

private:
    std::unordered_map<std::type_index, std::unique_ptr<SystemProfiler>> m_systemProfilers;
    std::unordered_map<std::type_index, std::unique_ptr<ISystem>> m_systems;
    std::vector<ISystem*> m_sequentialSystems;
};

template<typename T, typename... Args>
void SystemManager::addSystem(bool enableProfiling, Args&&... args)
{
    std::unique_ptr<ISystem> newSystem = std::make_unique<T>(std::forward<Args>(args)...);
    m_sequentialSystems.push_back(newSystem.get());
    m_systems.emplace(typeid(T), std::move(newSystem));

    if (enableProfiling)
        m_systemProfilers.emplace(typeid(T), std::make_unique<SystemProfiler>());
}

template<typename T>
T* SystemManager::getSystem()
{
    if (m_systems.count(typeid(T)))
        return static_cast<T*>(m_systems[typeid(T)].get());
    return nullptr;
}

template<typename T>
void SystemManager::removeSystem()
{
    if (m_systems.count(typeid(T)))
    {
        std::string_view type = m_systems[typeid(T)]->name();

        const auto it = std::find_if(m_sequentialSystems.begin(), m_sequentialSystems.end(), [type](ISystem* system) {
            return system->name() == type;
        });

        m_sequentialSystems.erase(it);
        m_systems.erase(typeid(T));
        m_systemProfilers.erase(typeid(T));
    }
}