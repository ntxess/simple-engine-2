#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <future>
#include <random>
#include <vector>

#include <SFML/Graphics/RectangleShape.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "ApplicationContext.hpp"
#include "Commands.hpp"
#include "Components.hpp"
#include "Managers.hpp"
#include "interface/IScene.hpp"
#include "interface/ISceneVisitor.hpp"
#include "util/DataStore.hpp"
#include "util/Logger.hpp"
#include "scene/Scenes.hpp"

using Grid = std::vector<uint8_t>;

class GameOfLifeSim final : public IScene
{
public:
    GameOfLifeSim();
    GameOfLifeSim(ApplicationContext* sysData);
    ~GameOfLifeSim();

    void init() override final;
    void processEvent(const sf::Event& event) override final;
    void processInput() override final;
    void update() override final;
    void render() override final;
    void pause() override final;
    void resume() override final;
    void setApplicationContext(ApplicationContext* context) override final;
    void accept(ISceneVisitor* visitor, entt::entity entityID) override final;
    entt::registry& getRegistry() override final;

    // Read-only access for snapshot/command-buffer renderers.
    const Grid& currentGrid() const;
    int gridWidth() const { return m_width; }
    int gridHeight() const { return m_height; }

private:
    const Grid& getCurrentGrid() const;

    inline int index(int x, int y) const
    {
        return y * m_width + x;
    }

    inline uint8_t getCell(const Grid& grid, int x, int y) const
    {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height) return 0;
        return grid[index(x, y)];
    }

private:
    ApplicationContext* m_appContext;
    entt::registry m_reg;

    int m_width;
    int m_height;
    Grid m_grids[2];
    std::atomic<int> m_currentReadBuffer{ 0 };
};