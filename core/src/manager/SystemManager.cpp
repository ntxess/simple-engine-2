#include "SystemManager.hpp"

void SystemManager::update(entt::registry& reg, const float& dt)
{
    for (const auto& [id, system] : m_systems)
    {
        if (m_systemProfilers.find(id) != m_systemProfilers.end())
        {
            m_systemProfilers.at(id)->timedUpdate(system.get(), reg, dt);
        }
        else
        {
            system->update(reg, dt);
        }
    }
}

std::unordered_map<std::string, std::deque<long long>> SystemManager::getSystemTimingHistory() const
{
    std::unordered_map<std::string, std::deque<long long>> historyMap;

    for (const auto& [sysType, profiler] : m_systemProfilers)
    {
        const auto& name = m_systems.at(sysType)->name();
        historyMap.emplace(name, profiler->getTimingHistory());
    }
    return historyMap;
}
