#pragma once

#include <SFML/Graphics/Vertex.hpp>
#include <cmath>
#include <memory>

class WayPoint
{
public:
	WayPoint();
	WayPoint(sf::Vector2f coordinate);

	WayPoint* next() const;
	WayPoint* link(std::unique_ptr<WayPoint> waypoint);

public:
	const sf::Vector2f coordinate;
	float distanceToNext;
	float distanceTotal;
	std::unique_ptr<WayPoint> nextWP;
};