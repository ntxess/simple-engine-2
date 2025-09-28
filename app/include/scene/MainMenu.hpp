#pragma once 

#include "ApplicationContext.hpp"
#include "Components.hpp"
#include "interface/IScene.hpp"
#include "interface/ISceneVisitor.hpp"
#include "util/DataStore.hpp"
#include "util/Logger.hpp"
#include "scene/Scenes.hpp"
#include "entt/entity/entity.hpp"
#include "entt/entity/registry.hpp"

class MainMenu final : public IScene
{
public:
    MainMenu();
    MainMenu(ApplicationContext* sysData);

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

private:
    ApplicationContext* m_appContext;
    entt::registry m_reg;

    entt::entity m_wallpaper;
};