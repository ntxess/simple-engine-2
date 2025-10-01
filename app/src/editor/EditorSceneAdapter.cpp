#include "EditorSceneAdapter.hpp"

EditorSceneAdapter::EditorSceneAdapter(std::unique_ptr<IScene> scn, unsigned int width, unsigned int height, const sf::ContextSettings& settings)
    : m_scene(std::move(scn))
{
    auto& reg = m_scene->getRegistry();
    setupComponentTrackers(reg);

    m_scene->init();

    m_renderTextureID = reg.create();
    reg.emplace<SceneViewRenderer>(m_renderTextureID, width, height, settings);
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

void EditorSceneAdapter::accept(ISceneVisitor* visitor, entt::entity entityID)
{
    m_scene->accept(visitor, entityID);
}

entt::registry& EditorSceneAdapter::getRegistry() const
{
    return m_scene->getRegistry();
}

sf::RenderTexture& EditorSceneAdapter::getRenderTexture() const
{
    return m_scene->getRegistry().get<SceneViewRenderer>(m_renderTextureID);
}

IScene* EditorSceneAdapter::get() const
{
    return m_scene.get();
}

entt::entity EditorSceneAdapter::createEntity()
{
    entt::entity entityID = m_scene->getRegistry().create();
    LOG_DEBUG(Logger::get()) << "Entity [" << static_cast<unsigned int>(entityID) << "] created";
    return entityID;
}

void EditorSceneAdapter::setupComponentTrackers(entt::registry& reg)
{
    // Update this whenever we add a new renderable component
    trackComponentType<Sprite>(reg);
    trackComponentType<UpdateEntityPolling>(reg);
    trackComponentType<UpdateEntityEvent>(reg);
    trackComponentType<EntityStatus>(reg);
    trackComponentType<EffectsList>(reg);
    trackComponentType<MovementPattern>(reg);
    trackComponentType<TeamTag>(reg);
}
