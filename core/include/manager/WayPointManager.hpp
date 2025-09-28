#pragma once

#include "util/WayPoint.hpp"
#include <rapidjson/document.h>
#include <fstream>
#include <sstream>
#include <unordered_map>

class WaypointManager
{
public:
	WaypointManager();

	void parseJsonData(std::string filename);
	void addWayPoint();
	WayPoint* getWayPoint(const std::string name);

private:
	std::unordered_map<std::string, std::unique_ptr<WayPoint>> wayPointMap;
};