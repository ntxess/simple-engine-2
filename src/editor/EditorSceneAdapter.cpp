#include "EditorSceneAdapter.hpp"

EditorSceneAdapter::EditorSceneAdapter(std::unique_ptr<IScene> scn, unsigned int width, unsigned int height, const sf::ContextSettings& settings)
    : m_scene(std::move(scn))
{
    m_scene->init();
    m_renderTextureID = m_scene->getRegistry().create();
    m_scene->getRegistry().emplace<SceneViewRenderer>(m_renderTextureID, width, height, settings);
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

