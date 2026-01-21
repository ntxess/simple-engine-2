#include "EditorSceneAdapter.hpp"

#include "scene/GameOfLifeSim.hpp"

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

void EditorSceneAdapter::update()
{
    m_scene->update();
}

void EditorSceneAdapter::buildSceneViewRenderCommands()
{
    auto& reg = m_scene->getRegistry();

    auto& out = m_sceneViewCmds.beginWrite();
    out.clear();

    // 1) Generic sprite gather (works for Sandbox/MainMenu).
    {
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
            out.sprites.push_back(cmd);
        }
    }

    // 2) Scene-specific emitters (extend as needed).
    if (auto* gol = dynamic_cast<GameOfLifeSim*>(m_scene.get()))
    {
        const Grid& grid = gol->currentGrid();

        // Emit 1x1 rectangles for alive cells (matches previous behavior).
        // Note: this is intentionally simple; a future optimization is to emit a vertex array or texture update.
        const sf::Color aliveColor{ 50, 168, 82 };
        const int w = gol->gridWidth();
        const int h = gol->gridHeight();

        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                if (grid[static_cast<std::size_t>(y * w + x)] == 1)
                {
                    out.rects.push_back(RectDrawCmd{
                        { static_cast<float>(x), static_cast<float>(y) },
                        { 1.f, 1.f },
                        aliveColor
                    });
                }
            }
        }
    }

    m_sceneViewCmds.publish();
}

void EditorSceneAdapter::drawSceneViewFromRenderCommands(sf::RenderTexture& target) const
{
    const auto& cmds = m_sceneViewCmds.acquireRead();

    m_sceneViewBatcher.begin();
    m_sceneViewBatcher.submit(cmds);
    m_sceneViewBatcher.flush(target);
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