#pragma once

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

#include "util/WayPoint.hpp"
#include <rapidjson/document.h>

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