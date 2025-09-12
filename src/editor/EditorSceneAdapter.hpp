#pragma once

#include "../core/Components.hpp"
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

private:
    void setupComponentTrackers(entt::registry& reg);

    template<typename T>
    void trackComponentType(entt::registry& reg)
    {
        LOG_INFO(Logger::get()) << "Tracking [" << typeid(T).name() << "] component";

        // The lambda signature must match: void(entt::registry&, entt::entity)
        reg.on_construct<T>().connect<&EditorSceneAdapter::onComponentConstruct<T>>(this);
        reg.on_destroy<T>().connect<&EditorSceneAdapter::onComponentDestroy<T>>(this);
    }

    template<typename T>
    void onComponentConstruct(entt::registry& registry, entt::entity entityID)
    {
        if (entities.find(entityID) == entities.end())
            entities.emplace(entityID, std::pair{ true, ComponentPropData{entityID} });
        entities.at(entityID).second.components.emplace_back(std::pair{ true, std::type_index(typeid(T)) });
        LOG_INFO(Logger::get()) << "Entity [" << static_cast<unsigned int>(entityID) << "] onComponentConstruct() triggered. Added [" << typeid(T).name() << "]";
    }

    template<typename T>
    void onComponentDestroy(entt::registry& registry, entt::entity entityID)
    {
        auto& components = entities.at(entityID).second.components;
        components.erase(
            std::remove_if(
                components.begin(),
                components.end(),
                [](const std::pair<bool, std::type_index>& comp) {
                    return comp.second == std::type_index(typeid(T));
                }
            ),
            components.end()
        );

        LOG_INFO(Logger::get()) << "Entity [" << static_cast<unsigned int>(entityID) << "] onComponentDestroy() triggered. Removed [" << typeid(T).name() << "]";
    }

public:
    // Assist rendering of Properties Panel
    std::unordered_map<entt::entity, std::pair<bool, ComponentPropData>> entities;
    std::unordered_map<std::type_index, std::function<void(entt::registry&, entt::entity)>> renderFunc;

private:
    entt::entity m_renderTextureID;
    std::unique_ptr<IScene> m_scene;
};

