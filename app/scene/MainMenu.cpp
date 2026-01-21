#include "MainMenu.hpp"

MainMenu::MainMenu()
    : m_appContext{nullptr}
    , m_wallpaper{entt::null}
{}

MainMenu::MainMenu(ApplicationContext* sysData)
    : m_appContext{sysData}
    , m_wallpaper{entt::null}
{}

MainMenu::~MainMenu()
{
    m_reg.clear();
}

void MainMenu::init()
{
    if (!m_appContext)
    {
        throw std::runtime_error("Application context not set for MainMenu scene.");
    }
    
    // Load the config file for texture paths and load them into the resource manager
    auto texturePaths = m_appContext->configDataSerializer.load("config/texture.toml");
    if (texturePaths)
    {
        for (const auto& [key, val] : texturePaths->lockedView())
        {
            try
            {
                auto path = std::filesystem::current_path().append(std::any_cast<std::string>(val));
                const auto pathStr = path.generic_string();
                if (!m_appContext->textureManager.load(key, pathStr, ResourceManager<sf::Texture, MutexSync>::ManagementStrategy::Reuse))
                {
                    LOG_ERROR(Logger::get()) << "Failed to load texture: " << path;
                }
            }
            catch (const std::bad_any_cast& e)
            {
                LOG_ERROR(Logger::get()) << "Invalid texture path for key: " << key << " - " << e.what();
            }
        }
    }
    else
    {
        LOG_ERROR(Logger::get()) << "Failed to load texture configuration file.";
    }

    m_wallpaper = m_reg.create();
    const auto& texture = m_appContext->textureManager.get("bg");
    if (!texture)
    {
        LOG_ERROR(Logger::get()) << "Failed to get texture for generated entity!";
        return;
    }
    m_reg.emplace<Sprite>(m_wallpaper, texture.value().get());
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
        auto& sceneRenderTexture = m_reg.get<SceneViewRenderer>(sceneTextureID);

        if (m_reg.all_of<Sprite>(m_wallpaper))
        {
            auto& spriteEntity = m_reg.get<Sprite>(m_wallpaper);
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