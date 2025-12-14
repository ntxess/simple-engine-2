#pragma once 

#include <random>

#include <SFML/Graphics/RectangleShape.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "ApplicationContext.hpp"
#include "Commands.hpp"
#include "Components.hpp"
#include "Managers.hpp"
#include "Systems.hpp"
#include "interface/IScene.hpp"
#include "interface/ISceneVisitor.hpp"
#include "util/DataStore.hpp"
#include "util/Logger.hpp"
#include "scene/Scenes.hpp"

class Sandbox final : public IScene
{
public:
    Sandbox();
    Sandbox(ApplicationContext* sysData);
    ~Sandbox();

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

    SystemManager* getSystemManager();

private:
    void checkBoundary(const sf::Vector2u& boundary, sf::Sprite& obj);

private:
    ApplicationContext* m_appContext;
    entt::entity m_player;
    SystemManager m_system;
    entt::registry m_reg;
    entt::registry m_collisionEventReg;
};