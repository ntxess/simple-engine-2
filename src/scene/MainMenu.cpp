#include "MainMenu.hpp"

MainMenu::MainMenu()
    : m_appContext(nullptr)
    , m_wallpaper(entt::null)
{}

MainMenu::MainMenu(ApplicationContext* sysData)
    : m_appContext(sysData)
    , m_wallpaper(entt::null)
{}

void MainMenu::init()
{
    DataStore texturePath;
    m_appContext->configDataSerializer.load("config/texture.json", texturePath);     // Load config file
    m_appContext->textureManager.load(texturePath, thor::Resources::Reuse); // Load the texture from loaded paths

    m_wallpaper = m_reg.create();
    m_reg.emplace<Sprite>(m_wallpaper, m_appContext->textureManager["bg"]);
}

void MainMenu::processEvent(const sf::Event& event)
{}

void MainMenu::processInput()
{}

void MainMenu::update()
{}

void MainMenu::render()
{
    const auto& scrView = m_reg.view<SceneViewRenderer>();
    for (const auto& sceneTextureID : scrView)
    {
        auto& sceneRenderTexture = m_reg.get<SceneViewRenderer>(sceneTextureID).rd;

        if (m_reg.all_of<Sprite>(m_wallpaper))
        {
            auto& spriteEntity = m_reg.get<Sprite>(m_wallpaper).sprite;
            sceneRenderTexture.draw(spriteEntity);
        }
    }
}

void MainMenu::pause()
{}

void MainMenu::resume()
{}

void MainMenu::setApplicationContext(ApplicationContext* context)
{
    m_appContext = context;
}

void MainMenu::accept(ISceneVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}

entt::registry& MainMenu::getRegistry()
{
    return m_reg;
}