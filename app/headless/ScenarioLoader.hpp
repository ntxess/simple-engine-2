#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <SFML/System/Vector2.hpp>

#include "ApplicationContext.hpp"
#include "component/EffectType.hpp"
#include "component/Team.hpp"

class Sandbox;

/**
 * @brief A Sandbox scenario for headless replays (legacy_replay --scenario), stored as JSON:
 *
 *     {
 *       "format": "se-legacy-scenario-v1",
 *       "name": "...", "description": "...",
 *       "eventWatchdogMs": 36000,                       (optional; EventSystem watchdog)
 *       "player": {                                     (optional overrides of Sandbox::init)
 *         "position": [x, y], "stats": {"HP": 100},
 *         "polling": {"cooldownMs": 1000, "readyOnStart": true},
 *         "effects": [ ...as below... ]                 (replaces the player's three effects)
 *       },
 *       "entities": [
 *         {
 *           "team": "ENEMY",                            (UNAFFILIATED, FRIENDLY or ENEMY)
 *           "texture": "player",                        (key in config/texture.json)
 *           "position": [x, y],                         (default: the first path point)
 *           "rotation": 0,                              (degrees)
 *           "stats": {"HP": 100, "Speed": 500},         (EntityStatus, in this order; Speed in px/s)
 *           "path": [[x, y], ...], "repeat": true,      (MovementPattern)
 *           "moving": true,                             (UpdateEntityEvent at spawn)
 *           "polling": {"cooldownMs": 1000, "readyOnStart": true},
 *           "effects": [{"type": "INSTANT", "stat": "HP", "value": -10,
 *                        "durationMs": 0, "intervalMs": 0}]
 *         }
 *       ]
 *     }
 *
 * Entities are spawned in file order after the player, so their SpawnIndex is 1, 2, ... and their
 * components are added in the order the editor's stress generator uses.
*/
struct ScenarioEffect
{
    EffectType type = EffectType::INSTANT;
    std::string stat = "HP";
    float value = 0.f;
    std::int64_t durationMs = 0;
    std::int64_t intervalMs = 0;
};

struct ScenarioPolling
{
    std::int64_t cooldownMs = 0;
    bool readyOnStart = true;
};

struct ScenarioEntity
{
    Team team = Team::ENEMY;
    std::string texture = "player";
    std::optional<sf::Vector2f> position;
    float rotation = 0.f;
    std::vector<std::pair<std::string, float>> stats;
    std::vector<sf::Vector2f> path;
    bool repeat = true;
    bool moving = true;
    std::optional<ScenarioPolling> polling;
    std::optional<std::vector<ScenarioEffect>> effects;
};

struct ScenarioPlayer
{
    std::optional<sf::Vector2f> position;
    std::vector<std::pair<std::string, float>> stats;
    std::optional<ScenarioPolling> polling;
    std::optional<std::vector<ScenarioEffect>> effects;
};

struct Scenario
{
    std::string name;
    std::string description;
    std::optional<std::int64_t> eventWatchdogMs;
    ScenarioPlayer player;
    std::vector<ScenarioEntity> entities;
};

namespace ScenarioLoader
{
    bool load(const std::string& path, Scenario& scenario, std::string& error);
    bool save(const std::string& path, const Scenario& scenario, std::string& error);

    // Applies the player overrides and spawns the entities into the Sandbox's registry.
    bool spawn(const Scenario& scenario, Sandbox& sandbox, ApplicationContext& ctx, std::string& error);

    // Enemies as the editor's stress generator creates them: HP 100, Speed 500 px/s, repeating path.
    void appendStressEntities(Scenario& scenario, const std::vector<std::vector<sf::Vector2f>>& paths);
}
