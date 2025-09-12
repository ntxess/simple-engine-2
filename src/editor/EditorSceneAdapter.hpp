#pragma once

#include "../core/component/SceneViewRenderer.hpp"
#include "../core/interface/IScene.hpp"
#include "../core/util/Logger.hpp"
#include "imgui.h"
#include "entt/entity/entity.hpp"
#include <functional>
#include <memory>
#include <typeindex>

struct ComponentPropData
{
    // Properties Panel
    entt::entity entityID;
    std::string name;
    std::vector<std::pair<bool, std::type_index>> components;

    // Waypoint Editor
    bool isWaypointEditorOpen;
    ImVector<ImVec2> points;
    ImVec2 scrolling;
    float zoom;

    ComponentPropData()
        : entityID(entt::null)
        , name("Entity NULL")
        , isWaypointEditorOpen(false)
        , scrolling(0.f, 0.f)
        , zoom(1.0f)
    {}

    ComponentPropData(entt::entity entity)
        : entityID(entity)
        , name("Entity " + std::to_string(static_cast<unsigned int>(entity)))
        , isWaypointEditorOpen(false)
        , scrolling(0.f, 0.f)
        , zoom(1.0f)
    {}
};

class EditorSceneAdapter
{
public:
    EditorSceneAdapter() = delete;
    EditorSceneAdapter(std::unique_ptr<IScene> scn, unsigned int width, unsigned int height, const sf::ContextSettings& settings);

    void processInput();
    void processEvent(const sf::Event& event);
    void render();
    void update();
    entt::registry& getRegistry() const;
    sf::RenderTexture& getRenderTexture() const;
    IScene* get() const;
    entt::entity createEntity();
    void createCompPropEntry(const entt::entity entityID);

public:
    // Assist rendering of Properties Panel
    std::unordered_map<entt::entity, std::pair<bool, ComponentPropData>> entities;
    std::unordered_map<std::type_index, std::function<void(entt::registry&, entt::entity)>> renderFunc;

private:
    entt::entity m_renderTextureID;
    std::unique_ptr<IScene> m_scene;
};

