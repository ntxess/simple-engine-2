#include "SystemManager.hpp"

void SystemManager::update(entt::registry& reg, const float& dt)
{
    // Execute systems in deterministic insertion order (important for correctness + profiling stability).
    for (const auto& entry : m_sequentialSystems)
    {
        const auto profilerIt = m_systemProfilers.find(entry.id);
        if (profilerIt != m_systemProfilers.end())
        {
            profilerIt->second->timedUpdate(entry.system, reg, dt);
        }
        else
        {
            entry.system->update(reg, dt);
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
