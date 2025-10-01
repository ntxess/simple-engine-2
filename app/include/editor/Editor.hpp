#pragma once

#include "editor/EditorComponentVisitor.hpp"
#include "editor/EditorSceneAdapter.hpp"
#include "editor/EditorSceneModifierVisitor.hpp"
#include "editor/EditorSceneSystemVisitor.hpp"
#include "ApplicationContext.hpp"
#include "Components.hpp"
#include "Systems.hpp"
#include "interface/IScene.hpp"
#include "util/Font.hpp"
#include "util/Logger.hpp"
#include "util/LogStream.hpp"
#include "scene/Scenes.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui-SFML.h"
#include "entt/entity/entity.hpp"
#include "entt/entity/registry.hpp"
#include <SFML/Graphics/Text.hpp>
#include <functional>
#include <memory>
#include <sstream>

class Editor : public IScene
{
private:
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
    void accept(ISceneVisitor* visitor, entt::entity entityID) override;
    entt::registry& getRegistry() override;

private:
    void setupComponentVisitors(IComponentVisitor *visitor);
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

    void generateEntities(size_t numOfEntities);

    template <typename... Args>
    entt::entity findEntityID();

    template <typename T>
    void registerComponentVisitor(IComponentVisitor* visitor, std::function<void(const entt::entity&)> callback = std::function<void(const entt::entity&)>{})
    {
        m_selectedSceneData->renderFunc[std::type_index(typeid(T))] = [visitor, callback](entt::registry& reg, const entt::entity entityID)
        {
            auto &component = reg.get<T>(entityID);
            component.accept(visitor, entityID);
            if (callback)
                callback(entityID);
        };
    }

private:
    ApplicationContext *m_appContext;

    LogStream m_logStream;
    Font m_defaultFont;
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

    std::atomic<bool> m_startButtonEnabled;
    std::atomic<bool> m_forwardFrameEnabled;

    // Current loaded scene data
    std::unordered_map<std::string, std::unique_ptr<EditorSceneAdapter>> m_editorSceneMap;
    std::string m_selectedSceneKey;
    EditorSceneAdapter *m_selectedSceneData;
    std::unique_ptr<sf::Sprite> m_gameView;

    EditorComponentVisitor m_componentVisitor;
    EditorSceneModifierVisitor m_sceneModifierVisitor;
    EditorSceneSystemVisitor m_sceneSystemVisitor;
};

template <typename... Args>
inline entt::entity Editor::findEntityID()
{
    // Hacky way of getting entity ID from a unique component
    const auto &view = m_selectedSceneData->getRegistry().view<Args...>();
    for (const auto &entityID : view)
    {
        return entityID;
    }

    return entt::null;
}