#pragma once

#include <SFML/Graphics/Vertex.hpp>
#include <cmath>
#include <memory>
#include <vector>

class WayPoint
{
public:
	WayPoint();
	WayPoint(sf::Vector2f coordinate);

	WayPoint* next() const;
	WayPoint* link(std::unique_ptr<WayPoint> waypoint);

	// Links the points into a path the way the editor's stress generator does, accumulating
	// distanceTotal; nullptr for no points.
	static std::unique_ptr<WayPoint> makePath(const std::vector<sf::Vector2f>& points);

public:
	const sf::Vector2f coordinate;
	float distanceToNext;
	float distanceTotal;
	std::unique_ptr<WayPoint> nextWP;
};