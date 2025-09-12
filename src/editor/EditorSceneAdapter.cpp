#include "EditorSceneAdapter.hpp"

EditorSceneAdapter::EditorSceneAdapter(std::unique_ptr<IScene> scn, unsigned int width, unsigned int height, const sf::ContextSettings& settings)
    : m_scene(std::move(scn))
{
    m_scene->init();
    m_renderTextureID = m_scene->getRegistry().create();
    m_scene->getRegistry().emplace<SceneViewRenderer>(m_renderTextureID, width, height, settings);

    // Sync the already generated entities to the map of ComponentPropData at start to prevent invalid index
    for (const auto& entityID : m_scene->getRegistry().view<entt::entity>())
    {
        createCompPropEntry(entityID);
    }
}

void EditorSceneAdapter::processInput()
{    
    m_scene->processInput();
}

void EditorSceneAdapter::processEvent(const sf::Event& event)
{
    m_scene->processEvent(event);
}

void EditorSceneAdapter::render()
{
    m_scene->render();
}

void EditorSceneAdapter::update()
{
    m_scene->update();
}

entt::registry& EditorSceneAdapter::getRegistry() const
{
    return m_scene->getRegistry();
}

sf::RenderTexture& EditorSceneAdapter::getRenderTexture() const
{
    return m_scene->getRegistry().get<SceneViewRenderer>(m_renderTextureID).rd;
}

IScene* EditorSceneAdapter::get() const
{
    return m_scene.get();
}

entt::entity EditorSceneAdapter::createEntity()
{
    entt::entity entityID = m_scene->getRegistry().create();
    LOG_DEBUG(Logger::get()) << "Entity [" << static_cast<unsigned int>(entityID) << "] created";
    createCompPropEntry(entityID);
    return entityID;
}

void EditorSceneAdapter::createCompPropEntry(const entt::entity entityID)
{
    LOG_INFO(Logger::get()) << "Syncing entity [" << static_cast<unsigned int>(entityID) << "] with ComponentPropData map";
    entities.emplace(entityID, std::pair{ true, ComponentPropData{entityID} });
}
