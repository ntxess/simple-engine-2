#pragma once

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Managers.hpp"
#include "interface/ISystem.hpp"
#include "util/SystemProfiler.hpp"

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
    struct SystemEntry
    {
        std::type_index id;
        ISystem* system;
    };

    std::unordered_map<std::type_index, std::unique_ptr<SystemProfiler>> m_systemProfilers;
    std::unordered_map<std::type_index, std::unique_ptr<ISystem>> m_systems;
    std::vector<SystemEntry> m_sequentialSystems;
};

template<typename T, typename... Args>
void SystemManager::addSystem(bool enableProfiling, Args&&... args)
{
    std::unique_ptr<ISystem> newSystem = std::make_unique<T>(std::forward<Args>(args)...);
    const std::type_index id = typeid(T);
    m_sequentialSystems.push_back(SystemEntry{ id, newSystem.get() });
    m_systems.emplace(id, std::move(newSystem));

    if (enableProfiling)
        m_systemProfilers.emplace(id, std::make_unique<SystemProfiler>());
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
        const std::type_index id = typeid(T);
        const auto it = std::find_if(m_sequentialSystems.begin(), m_sequentialSystems.end(), [id](const SystemEntry& entry) {
            return entry.id == id;
        });

        if (it != m_sequentialSystems.end())
            m_sequentialSystems.erase(it);

        m_systems.erase(id);
        m_systemProfilers.erase(id);
    }
}