#include "SystemProfiler.hpp"

SystemProfiler::SystemProfiler(size_t historyLen)
	: m_historyLen(historyLen)
{}

void SystemProfiler::timedUpdate(ISystem* system, entt::registry& reg, const float& dt)
{
	auto start = std::chrono::high_resolution_clock::now();
	system->update(reg, dt);
	auto stop = std::chrono::high_resolution_clock::now();

	long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();

	m_timingHistory.push_back(ns);
	if (m_timingHistory.size() > m_historyLen)
		m_timingHistory.pop_front();
}

long long SystemProfiler::getLastDuration() const
{
	return m_timingHistory.empty() ? 0 : m_timingHistory.back();
}

const std::deque<long long>& SystemProfiler::getTimingHistory() const
{
	return m_timingHistory;
}