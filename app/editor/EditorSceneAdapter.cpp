#include "EditorSceneAdapter.hpp"

#include <SFML/Graphics/Sprite.hpp>

#include "scene/Sandbox.hpp"

EditorSceneAdapter::EditorSceneAdapter(std::unique_ptr<IScene> scn, unsigned int width, unsigned int height, const sf::ContextSettings& settings)
    : m_scene(std::move(scn))
    , m_enableSceneViewCommands{false}
{
    auto& reg = m_scene->getRegistry();
    setupComponentTrackers(reg);

    m_scene->init();

    m_renderTextureID = reg.create();
    reg.emplace<SceneViewRenderer>(m_renderTextureID, width, height, settings);

    // Start with Sandbox only (lowest risk) and expand to more scenes once the pattern is proven.
    m_enableSceneViewCommands = (dynamic_cast<Sandbox*>(m_scene.get()) != nullptr);
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

bool EditorSceneAdapter::shouldUseSceneViewCommands() const
{
    return m_enableSceneViewCommands;
}

void EditorSceneAdapter::buildSceneViewCommands()
{
    if (!m_enableSceneViewCommands)
        return;

    auto& reg = m_scene->getRegistry();

    auto& out = m_sceneViewSpriteCmds.beginWrite();
    out.clear();

    const auto view = reg.view<Sprite>();
    for (const auto entity : view)
    {
        const auto& sp = view.get<Sprite>(entity);

        SpriteDrawCmd cmd;
        cmd.texture = &sp.getTexture();
        cmd.texRect = sp.getTextureRect();
        cmd.position = sp.getPosition();
        cmd.scale = sp.getScale();
        cmd.origin = sp.getOrigin();
        cmd.color = sp.getColor();
        cmd.rotationDeg = sp.getRotation().asDegrees();
        out.push_back(cmd);
    }

    m_sceneViewSpriteCmds.publish();
}

void EditorSceneAdapter::renderSceneViewFromCommands(sf::RenderTexture& target) const
{
    if (!m_enableSceneViewCommands)
        return;

    const auto& cmds = m_sceneViewSpriteCmds.acquireRead();
    for (const auto& cmd : cmds)
    {
        if (!cmd.texture) continue;

        sf::Sprite sprite(*cmd.texture, cmd.texRect);
        sprite.setPosition(cmd.position);
        sprite.setScale(cmd.scale);
        sprite.setOrigin(cmd.origin);
        sprite.setColor(cmd.color);
        sprite.setRotation(sf::degrees(cmd.rotationDeg));
        target.draw(sprite);
    }
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