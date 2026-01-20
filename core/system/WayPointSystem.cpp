#include "WayPointSystem.hpp"

#include <cmath>
#include <numbers>
#include <vector>

#include "util/TaskGroup.hpp"
#include "util/TaskScheduler.hpp"

#include "component/EntityStatus.hpp"
#include "component/MovementPattern.hpp"
#include "component/Sprite.hpp"
#include "component/UpdateEntityEvent.hpp"

WayPointSystem::WayPointSystem(std::string bindingStatID, TaskScheduler* jobScheduler)
    : m_bindingStatID{bindingStatID}
    , m_jobScheduler{jobScheduler}
{}


void WayPointSystem::update(entt::registry& reg, const float& dt)
{
    const auto group = reg.group<MovementPattern, EntityStatus>(entt::get<Sprite>);

    // If no job scheduler is available, run sequentially (original behavior).
    if (m_jobScheduler == nullptr)
    {
        for (const auto& entityID : group)
        {
            auto&& [wpc, es, sp] = group.get<MovementPattern, EntityStatus, Sprite>(entityID);

            const auto speedIt = es.values.find(m_bindingStatID);
            if (speedIt == es.values.end()) continue;

            const float speed = speedIt->second;

            WayPoint* headPtr = wpc.currentPath;
            WayPoint* nextPtr = (headPtr ? headPtr->next() : nullptr);

            if (nextPtr == nullptr)
            {
                if (wpc.repeat)
                {
                    wpc.currentPath = wpc.movePattern.get();
                    wpc.distance = 0.f;
                }
                continue;
            }

            wpc.distance += speed * dt;
            if (wpc.distance > nextPtr->distanceTotal)
                wpc.currentPath = nextPtr;

            const float denom = headPtr->distanceToNext;
            if (denom == 0.0f) continue;

            sf::Vector2f unitDist;
            unitDist.x = (nextPtr->coordinate.x - headPtr->coordinate.x) / denom;
            unitDist.y = (nextPtr->coordinate.y - headPtr->coordinate.y) / denom;

            sf::Vector2f velocity;
            velocity.x = unitDist.x * speed * dt;
            velocity.y = unitDist.y * speed * dt;

            const float heading = (std::atan2(velocity.y, velocity.x) * (180.f / float(std::numbers::pi))) + 90.f;
            sp.setRotation(sf::degrees(heading));
            sp.move(velocity);
            reg.emplace_or_replace<UpdateEntityEvent>(entityID);
        }

        return;
    }

    // Jobified path:
    // - Gather required data single-threaded (safe EnTT iteration).
    // - Compute movement in parallel (pure math, no registry mutation).
    // - Apply results single-threaded (safe EnTT mutation).
    struct ItemIn
    {
        entt::entity id{};
        float speed{};
        bool repeat{};
        float prevDistance{};
        sf::Vector2f prevPos{};
        WayPoint* head{};
        WayPoint* next{};
        WayPoint* root{};
    };

    struct ItemOut
    {
        bool dirty{false};
        bool reset{false};
        float newDistance{0.f};
        WayPoint* newCurrent{nullptr};
        sf::Vector2f newPos{};
        float headingDeg{0.f};
    };

    std::vector<ItemIn> inputs;
    inputs.reserve(static_cast<std::size_t>(group.size()));

    for (const auto& entityID : group)
    {
        auto&& [wpc, es, sp] = group.get<MovementPattern, EntityStatus, Sprite>(entityID);

        const auto speedIt = es.values.find(m_bindingStatID);
        if (speedIt == es.values.end()) continue;

        ItemIn in;
        in.id = entityID;
        in.speed = speedIt->second;
        in.repeat = wpc.repeat;
        in.prevDistance = wpc.distance;
        in.prevPos = sp.getPosition();
        in.head = wpc.currentPath;
        in.next = (wpc.currentPath ? wpc.currentPath->next() : nullptr);
        in.root = wpc.movePattern.get();
        inputs.push_back(in);
    }

    std::vector<ItemOut> outputs(inputs.size());

    // Chunk size tuned to keep overhead low for large entity counts.
    constexpr std::size_t grain = 256;

    parallel_for(*m_jobScheduler, inputs.size(), grain, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i)
        {
            const ItemIn& in = inputs[i];
            ItemOut& out = outputs[i];

            if (in.head == nullptr)
                continue;

            if (in.next == nullptr)
            {
                if (in.repeat && in.root != nullptr)
                {
                    out.reset = true;
                    out.newCurrent = in.root;
                    out.newDistance = 0.f;
                }
                continue;
            }

            const float denom = in.head->distanceToNext;
            if (denom == 0.0f)
                continue;

            const float newDistance = in.prevDistance + in.speed * dt;

            // Advance current waypoint if we've passed the next node.
            out.newCurrent = (newDistance > in.next->distanceTotal) ? in.next : in.head;
            out.newDistance = newDistance;

            const float unitX = (in.next->coordinate.x - in.head->coordinate.x) / denom;
            const float unitY = (in.next->coordinate.y - in.head->coordinate.y) / denom;

            const float velX = unitX * in.speed * dt;
            const float velY = unitY * in.speed * dt;

            out.newPos = { in.prevPos.x + velX, in.prevPos.y + velY };
            out.headingDeg = (std::atan2(velY, velX) * (180.f / float(std::numbers::pi))) + 90.f;
            out.dirty = true;
        }
    });

    for (std::size_t i = 0; i < inputs.size(); ++i)
    {
        const auto entityID = inputs[i].id;
        auto&& [wpc, _es, sp] = group.get<MovementPattern, EntityStatus, Sprite>(entityID);
        const ItemOut& out = outputs[i];

        if (out.reset)
        {
            wpc.currentPath = out.newCurrent;
            wpc.distance = 0.f;
            continue;
        }

        if (!out.dirty)
            continue;

        wpc.currentPath = out.newCurrent;
        wpc.distance = out.newDistance;
        sp.setRotation(sf::degrees(out.headingDeg));
        sp.setPosition(out.newPos);

        reg.emplace_or_replace<UpdateEntityEvent>(entityID);
    }
}