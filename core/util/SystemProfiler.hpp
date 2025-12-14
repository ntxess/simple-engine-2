#pragma once

#include <cstddef>
#include <deque>

#include <entt/entity/registry.hpp>

#include "interface/ISystem.hpp"

class SystemProfiler
{
public:
	explicit SystemProfiler(size_t historyLen = 120);

	void timedUpdate(ISystem* system, entt::registry& reg, const float& dt);
	long long getLastDuration() const;
	const std::deque<long long>& getTimingHistory() const;

private:
	size_t m_historyLen;
	std::deque<long long> m_timingHistory;
};

