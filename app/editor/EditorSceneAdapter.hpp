#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SFML/Graphics/RenderTexture.hpp>
#include <SFML/Window/ContextSettings.hpp>
#include <SFML/Window/Event.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>
#include <imgui.h>

#include "Components.hpp"
#include "interface/IScene.hpp"
#include "interface/ISceneVisitor.hpp"
#include "util/SpriteCommandBuffer.hpp"
#include "util/Logger.hpp"

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
        : entityID{entt::null}
        , name{"Entity NULL"}
        , isWaypointEditorOpen{false}
        , scrolling{0.f, 0.f}
        , zoom{1.0f}
    {}

    ComponentPropData(entt::entity entity)
        : entityID{entity}
        , name{"Entity " + std::to_string(static_cast<unsigned int>(entity))}
        , isWaypointEditorOpen{false}
        , scrolling{0.f, 0.f}
        , zoom{1.0f}
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

    // Snapshot / command-buffer render for the Scene View panel.
    // buildSceneViewCommands() must be called from the sim thread (safe to read registry).
    // renderSceneViewFromCommands() must be called from the render thread (SFML draw calls).
    void buildSceneViewCommands();
    void renderSceneViewFromCommands(sf::RenderTexture& target) const;
    bool shouldUseSceneViewCommands() const;
    void accept(ISceneVisitor* visitor, entt::entity entityID);

    entt::registry& getRegistry() const;
    sf::RenderTexture& getRenderTexture() const;
    IScene* get() const;
    entt::entity createEntity();

private:
    template<typename T>
    void trackComponentType(entt::registry& reg)
    {
        LOG_INFO(Logger::get()) << "Tracking [" << typeid(T).name() << "] component";

        // The lambda signature must match: void(entt::registry&, entt::entity)
        reg.on_construct<T>().template connect<&EditorSceneAdapter::onComponentConstruct<T>>(*this);
        reg.on_destroy<T>().template connect<&EditorSceneAdapter::onComponentDestroy<T>>(*this);
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

    void setupComponentTrackers(entt::registry& reg);

public:
    // Assist rendering of Properties Panel
    std::unordered_map<entt::entity, std::pair<bool, ComponentPropData>> entities;
    std::unordered_map<std::type_index, std::function<void(entt::registry&, entt::entity)>> renderFunc;

private:
    entt::entity m_renderTextureID;
    std::unique_ptr<IScene> m_scene;
    bool m_enableSceneViewCommands;
    TripleBufferedSpriteCommands m_sceneViewSpriteCmds;
};

