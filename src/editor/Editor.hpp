#pragma once

#include "EditorComponentVisitor.hpp"
#include "EditorSceneAdapter.hpp"
#include "EditorSceneVisitor.hpp"
#include "../core/ApplicationContext.hpp"
#include "../core/Components.hpp"
#include "../core/Systems.hpp"
#include "../core/interface/IScene.hpp"
#include "../core/util/Logger.hpp"
#include "../core/util/LogStream.hpp"
#include "../scene/Scenes.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui-SFML.h"
#include "entt/entity/entity.hpp"
#include "entt/entity/registry.hpp"
#include "SFML/Graphics/Font.hpp"
#include "SFML/Graphics/Text.hpp"
#include <memory>

class Editor : public IScene
{
private:
    struct ComponentPropData
    {
        entt::entity entityID{};
        std::string name;
        std::array<bool, 7> closableComponents{};

        // Waypoint Editor
        bool isWaypointEditorOpen{ false };
        ImVector<ImVec2> points;
        ImVec2 scrolling{ 0.f, 0.f };
        float zoom = 1.0f;

        ComponentPropData() = default;
        ComponentPropData(entt::entity entityID)
            : entityID(entityID)
            , name("Entity " + std::to_string(static_cast<unsigned int>(entityID)))
        {
            closableComponents.fill(true);
        }
    };

    enum class WayPointEditMode
    {
        Add,
        RemoveLast,
        Clear
    };

public:
    Editor();
    Editor(ApplicationContext* sysData);
    ~Editor();

    void init() override;
    void processEvent(const sf::Event& event) override;
    void processInput() override;
    void update() override;
    void render() override;
    void pause() override;
    void resume() override;
    void setApplicationContext(ApplicationContext* context) override;
    void accept(ISceneVisitor* visitor) override;
    entt::registry& getRegistry() override;

private:
    void setupDockPanel(const ImVec2& panPos, const ImVec2& panSize, const char* panID, const ImGuiID& dockID) const;
    void renderDebugPanel(const ImVec2& pos, const ImVec2& size);
    void renderPerformancePanel(const ImVec2& pos, const ImVec2& size);
    void renderLogViewPanel(const ImVec2& pos, const ImVec2& size);
    void renderSceneViewPanel(const ImVec2& pos, const ImVec2& size);
    void renderAssetsExplorerPanel(const ImVec2& pos, const ImVec2& size);
    void renderPropertiesPanel(const ImVec2& pos, const ImVec2& size);
    void displayEntityVisualizers();
    void displayCollisionSystemVisualizer();

    ImVec2 scaleSize(const ImVec2& region, const float& aspectRatio);
    ImVec2 getCenteredTLPos(const ImVec2& region, const float& aspectRatio);

    // WayPoint Canvas
    void wayPointCanvasCallback(const entt::entity& entityID);
    void drawWayPointCanvas(const entt::entity& entityID, ComponentPropData& cmpntData);
    void drawWayPoints(const ImVec2& tlBound, const ImVec2& brBound, const ImVec2& size, const ImVec2& origin, ComponentPropData& cmpntData);
    void drawWayPointContextMenu(const entt::entity& entityID, ComponentPropData& cmpntData);
    void updateWayPointCanvas(const entt::entity& entityID, ComponentPropData& cmpntData);
    void updateWayPointComponent(const entt::entity& entityID, ComponentPropData& cmpntData, WayPointEditMode mode);
    void processWayPointCanvasInput(const ImVec2& size, const ImVec2& origin, const entt::entity& entityID, ComponentPropData& cmpntData);

    template<typename... Args>
    entt::entity findEntityID();

    template<typename T>
    bool renderComponentProperties(
        const entt::entity& entityID, 
        std::string_view componentID, 
        bool& visible, 
        EditorComponentVisitor& visitor,
        std::function<void(const entt::entity&)> callback = nullptr
    );

private:
    ApplicationContext* m_appContext;

    LogStream m_logStream;
    sf::Font m_defaultFont;
    ImGuiWindowFlags m_panelFlags;

    ImGuiID m_dockspaceId1;
    ImGuiID m_dockspaceId2;
    ImGuiID m_dockspaceId3;
    ImGuiID m_dockspaceId4;
    ImGuiID m_dockspaceId5;

    bool m_enableEntityID;
    bool m_enableEntityCollider;
    bool m_enableEntityHeading;
    bool m_enableEntityPosition;
    bool m_enableQuadTreeVisualizer;
    bool m_enableLogViewer;
    ImVec2 m_sceneDrawScale;
    int m_totalEntity;

    std::atomic<bool> m_startButtonEnabled;
    std::atomic<bool> m_forwardFrameEnabled;

    // Current loaded scene data
    std::unordered_map<std::string, std::unique_ptr<EditorSceneAdapter>> m_editorSceneMap;
    std::string m_selectedSceneKey;
    EditorSceneAdapter* m_selectedSceneData;
    sf::Sprite m_gameView;

    // Component Data used for modifying properties in PropertiesPanel
    std::unordered_map<entt::entity, ComponentPropData> m_entities;

    EditorComponentVisitor m_componentVisitor;
    EditorSceneVisitor m_sceneVisitor;
};

template<typename... Args>
inline entt::entity Editor::findEntityID()
{
    // Hacky way of getting entity ID from a unique component
    const auto& view = m_selectedSceneData->getRegistry().view<Args...>();
    for (const auto& entityID : view)
    {
        return entityID;
    }

    return entt::null;
}

template<typename T>
inline bool Editor::renderComponentProperties(const entt::entity& entityID, std::string_view componentID, bool& visible, EditorComponentVisitor& visitor, std::function<void(const entt::entity&)> callback)
{
    if (!m_selectedSceneData->getRegistry().all_of<T>(entityID))
    {
        return false;
    }

    if (ImGui::CollapsingHeader(componentID.data(), &visible, ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto& component = m_selectedSceneData->getRegistry().get<T>(entityID);
        component.accept(&visitor, entityID);

        if (callback)
        {
            callback(entityID);
        }
    }

    if (!visible)
    {
        // TODO: Remove the sprite component from QuadTree
        m_selectedSceneData->getRegistry().remove<T>(entityID);
        //static_cast<Sandbox*>(
        // m_sceneMap[m_selectedSceneKey]->scene.get())->getSystemManager()->getSystem<CollisionSystem>()->remove(*m_reg, entityID);
        return false;
    }

    return true;
}
