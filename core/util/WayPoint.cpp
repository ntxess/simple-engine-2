#include "WayPoint.hpp"

WayPoint::WayPoint()
	: coordinate{sf::Vector2f(0.f, 0.f)}
	, distanceToNext{0.f}
	, distanceTotal{0.f}
	, nextWP{nullptr}
{}

WayPoint::WayPoint(sf::Vector2f coordinate)
	: coordinate{coordinate}
	, distanceToNext{0.f}
	, distanceTotal{0.f}
	, nextWP{nullptr}
{}

WayPoint* WayPoint::next() const
{ 
	return nextWP.get();
}

std::unique_ptr<WayPoint> WayPoint::makePath(const std::vector<sf::Vector2f>& points)
{
	std::unique_ptr<WayPoint> root = nullptr;
	WayPoint* last = nullptr;

	for (size_t j = 0; j < points.size(); j++)
	{
		std::unique_ptr<WayPoint> point = std::make_unique<WayPoint>(points[j]);

		if (j != 0)
		{
			last->link(std::move(point));
			last->nextWP->distanceTotal = last->distanceTotal + last->distanceToNext;
			last = last->next();
		}
		else
		{
			root = std::move(point);
			last = root.get();
		}
	}

	return root;
}

WayPoint* WayPoint::link(std::unique_ptr<WayPoint> waypoint)
{
	nextWP = std::move(waypoint);

	distanceToNext = sqrt(
		(nextWP->coordinate.x - coordinate.x) * 
		(nextWP->coordinate.x - coordinate.x) +
		(nextWP->coordinate.y - coordinate.y) *
		(nextWP->coordinate.y - coordinate.y)
	);

	return nextWP.get();
}
