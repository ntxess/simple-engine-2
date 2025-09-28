#pragma once

#include "interface/ISystem.hpp"
#include "entt/entity/entity.hpp"
#include "entt/entity/registry.hpp"
#include <chrono>
#include <deque>
#include <memory>
#include <string_view>

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

