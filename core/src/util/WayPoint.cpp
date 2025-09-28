#include "WayPoint.hpp"

WayPoint::WayPoint()
	: coordinate(sf::Vector2f(0.f, 0.f))
	, distanceToNext(0.f)
	, distanceTotal(0.f)
	, nextWP(nullptr)
{}

WayPoint::WayPoint(sf::Vector2f coordinate)
	: coordinate(coordinate)
	, distanceToNext(0.f)
	, distanceTotal(0.f)
	, nextWP(nullptr)
{}

WayPoint* WayPoint::next() const
{ 
	return nextWP.get();
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
