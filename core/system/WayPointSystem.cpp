#include "WayPointSystem.hpp"

#include <cstddef>
#include <limits>

namespace
{
    // The waypoint at `index` along the path (0 is the first), or nullptr past the last one.
    const WayPoint* waypointAt(const WayPoint* head, std::size_t index)
    {
        const WayPoint* point = head;
        for (std::size_t i = 0; point != nullptr && i < index; ++i)
            point = point->next();
        return point;
    }

    std::size_t waypointCount(const WayPoint* head)
    {
        std::size_t count = 0;
        for (const WayPoint* point = head; point != nullptr; point = point->next())
            ++count;
        return count;
    }
}

WayPointSystem::WayPointSystem(std::string bindingStatID)
    : m_bindingStatID{bindingStatID}
{}

constexpr std::string_view WayPointSystem::name() const
{
    return "WayPointSystem";
}

void WayPointSystem::update(entt::registry& reg, const float& dt)
{
    const auto& group = reg.group<MovementPattern, EntityStatus>(entt::get<Sprite>);
    for (const auto& entityID : group)
    {
        auto [wpc, es, sp] = group.get<MovementPattern, EntityStatus, Sprite>(entityID);

        const auto speedIt = es.values.find(m_bindingStatID);
        if (speedIt == es.values.end()) continue;

        const WayPoint* head = wpc.movePattern.get();
        if (head == nullptr || wpc.finished) continue;

        // Rule R12: the progress is the current segment (from waypoint `segment` to the next) and
        // the distance travelled along it, and the position is taken from the path instead of
        // being integrated. The float expressions are evaluated as written, in this order, as the
        // engine does (docs/rules/gameplay-rules.md). Rule R13: a repeating path has one more
        // segment, which closes the loop from its last point back to its first.
        const std::size_t count = waypointCount(head);
        const std::size_t segments = wpc.repeat ? count : count - 1;

        std::size_t segment = wpc.segment;
        float u = wpc.segmentDistance;
        const WayPoint* from = waypointAt(head, segment);
        if (segment >= segments || from == nullptr)
        {
            // Out of range only after an edit that shortened the path: start over.
            segment = 0;
            u = 0.f;
            from = head;
        }

        float rem = speedIt->second * dt;
        float remAtWrap = std::numeric_limits<float>::infinity();
        std::size_t zeroRun = 0;
        bool reachedEnd = false;

        // The last segment of positive length the entity moved along this tick, which places and
        // turns it.
        const WayPoint* moveFrom = nullptr;
        const WayPoint* moveTo = nullptr;
        float moveLength = 0.f;

        while (true)
        {
            if (segment == segments)
            {
                // No segment left: only a non-repeating path ends, on its last point.
                reachedEnd = true;
                break;
            }

            const WayPoint* to = from->next();
            float len = from->distanceToNext;
            if (to == nullptr)
            {
                // Rule R13: the closing segment of a repeating path.
                to = head;
                len = WayPoint::distanceBetween(from->coordinate, head->coordinate);
            }
            if (len <= 0.f)
            {
                // Rule R6: a zero-length segment is skipped without moving or turning. A repeating
                // path whose points all coincide (all its segments have length 0) never moves.
                if (wpc.repeat && ++zeroRun == segments) break;
            }
            else
            {
                zeroRun = 0;
                moveFrom = from;
                moveTo = to;
                moveLength = len;
                if (u + rem < len)
                {
                    u = u + rem;
                    break;
                }
                rem = (u + rem) - len;
            }

            // On to the next segment.
            u = 0.f;
            ++segment;
            from = to;
            if (segment == segments && wpc.repeat)
            {
                // Past the closing segment (which ended on the first point): start over.
                segment = 0;

                // A step so long that a whole round no longer shortens it in float would go round
                // forever (or a NaN or infinite speed): it ends at the start of the path instead.
                if (!(rem < remAtWrap)) rem = 0.f;
                remAtWrap = rem;
            }
        }

        wpc.segment = segment;
        wpc.segmentDistance = u;

        sf::Vector2f position = sp.getPosition();
        if (reachedEnd)
        {
            // Exactly on the last point, and the path is done for good.
            position = from->coordinate;
            wpc.finished = true;
        }
        else if (moveFrom != nullptr)
        {
            const float t = u / moveLength;
            position.x = moveFrom->coordinate.x + (moveTo->coordinate.x - moveFrom->coordinate.x) * t;
            position.y = moveFrom->coordinate.y + (moveTo->coordinate.y - moveFrom->coordinate.y) * t;
        }

        if (moveFrom != nullptr)
        {
            const float dx = moveTo->coordinate.x - moveFrom->coordinate.x;
            const float dy = moveTo->coordinate.y - moveFrom->coordinate.y;
            const float heading = (std::atan2(dy, dx) * (180.f / float(std::numbers::pi))) + 90.f;
            sp.setRotation(sf::degrees(heading));
        }

        // Moved (the dirty flag of rules R2 and R8) on every tick the position changes.
        if (position != sp.getPosition())
        {
            sp.setPosition(position);
            reg.emplace_or_replace<UpdateEntityEvent>(entityID);
        }
    }
}
