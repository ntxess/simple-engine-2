#include "Sandbox.hpp"

Sandbox::Sandbox()
    : m_appContext{nullptr}
    , m_player{entt::null}
{}

Sandbox::Sandbox(ApplicationContext* sysData)
    : m_appContext{sysData}
    , m_player{entt::null}
{}

Sandbox::~Sandbox()
{
    m_reg.clear();
}

void Sandbox::init()
{
    if (!m_appContext)
    {
        throw std::runtime_error("Application context not set for Sanbox scene.");
    }

    // Load the config file for texture paths and load them into the resource manager
    auto texturePaths = m_appContext->configDataSerializer.load("config/texture.json");
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

    // Create the main player object
    m_player = m_reg.create();
    const auto& texture = m_appContext->textureManager.get("player");
    if (!texture)
    {
        LOG_ERROR(Logger::get()) << "Failed to get texture for generated entity!";
        return;
    }
    m_reg.emplace<Sprite>(m_player, texture.value().get());
    m_reg.emplace<TeamTag>(m_player, Team::FRIENDLY);
    m_reg.emplace<PlayerInput>(m_player);
    m_reg.emplace<EffectsList>(m_player);
    m_reg.emplace<UpdateEntityPolling>(m_player, std::chrono::milliseconds(1000), true);
    m_reg.emplace<EntityStatus>(m_player);
    m_reg.get<EntityStatus>(m_player).values["HP"] = 100.f;
    m_reg.get<PlayerInput>(m_player).input =
    {
        { sf::Keyboard::Scancode::W, new Movement(m_player, { 0, -1 }) },
        { sf::Keyboard::Scancode::A, new Movement(m_player, { -1, 0 }) },
        { sf::Keyboard::Scancode::S, new Movement(m_player, { 0,  1 }) },
        { sf::Keyboard::Scancode::D, new Movement(m_player, { 1,  0 }) }
    };

    float width = static_cast<float>(m_appContext->configData.get<int>("width").value());
    float height = static_cast<float>(m_appContext->configData.get<int>("height").value());

    m_reg.get<Sprite>(m_player).setPosition({200, 200});

    // Create event effect for collecting coins
    m_reg.get<EffectsList>(m_player).effectsList.push_back({ EffectType::INSTANT, Effects{"HP", -10.f} });
    m_reg.get<EffectsList>(m_player).effectsList.push_back({
        EffectType::OVERTIME,
        Effects{"HP",
        -1.f,
        std::chrono::milliseconds(5000),
        std::chrono::milliseconds(1000)}
        });
    m_reg.get<EffectsList>(m_player).effectsList.push_back({ EffectType::TEMPTIMED, Effects{"HP", -10.f, std::chrono::milliseconds(5000)} });

    m_system.addSystem<CollisionSystem>(true, m_collisionEventReg, sf::Vector2f{ 0.f, 0.f }, m_appContext->window.getSize());
    m_system.addSystem<EventSystem>(true, m_collisionEventReg, std::chrono::milliseconds(36000));
    m_system.addSystem<WayPointSystem>(true, "Speed");
}

void Sandbox::processEvent(const sf::Event& event)
{
    if (const auto* keyPressed = event.getIf<sf::Event::KeyPressed>())
    {
        if (keyPressed->scancode == sf::Keyboard::Scancode::Escape)
            m_appContext->sceneManager.addScene(std::make_unique<MainMenu>(m_appContext));

        //auto& controller = m_reg.get<PlayerInput>(m_player);
        //for (auto& [key, action] : controller.input)
        //	if (event.key.code == key)
        //		action->execute(m_reg);
    }
}

void Sandbox::processInput()
{
    auto& controller = m_reg.get<PlayerInput>(m_player);
    for (auto& [key, action] : controller.input)
        if (sf::Keyboard::isKeyPressed(key))
            action->execute(m_reg);

    //m_reg.get<PlayerInput>(m_player).processInput(m_reg);
}

void Sandbox::update()
{
    LOG_TRACE(Logger::get()) << "Entering update()";

    m_system.update(m_reg, m_appContext->deltaTime);

    // Delete anything that has zero or less HP
    const auto& view = m_reg.view<EntityStatus>();
    for (const auto& entity : view)
    {
        if (m_reg.get<EntityStatus>(entity).getOr("HP", 0.f) <= 0.f)
        {
            LOG_INFO(Logger::get()) << "Destroying entity [" << static_cast<unsigned int>(entity) << "]";

            m_system.getSystem<CollisionSystem>()->remove(m_reg, entity);
            m_reg.destroy(entity);
        }
    }

    LOG_TRACE(Logger::get()) << "Leaving update()";
}

void Sandbox::render()
{
    LOG_TRACE(Logger::get()) << "Entering render()";

    const auto& scrView = m_reg.view<SceneViewRenderer>();
    for (const auto& sceneTextureID : scrView)
    {
        const auto& view = m_reg.view<Sprite>();
        for (const auto& entity : view)
        {
            if (m_reg.valid(entity))
            {
                auto& sceneRenderTexture = m_reg.get<SceneViewRenderer>(sceneTextureID);
                auto& spriteEntity = view.get<Sprite>(entity);
                checkBoundary(sceneRenderTexture.getSize(), spriteEntity);
                sceneRenderTexture.draw(view.get<Sprite>(entity));
            }
        }
    }

    //const auto& view = m_reg.view<Sprite>();
    //for (const auto& entity : view)
    //{
    //    auto& spriteEntity = view.get<Sprite>(entity).sprite;
    //    checkBoundary(m_appContext->window.getSize(), spriteEntity);
    //    m_appContext->window.draw(view.get<Sprite>(entity).sprite);
    //}

    LOG_TRACE(Logger::get()) << "Leaving render()";
}

void Sandbox::pause()
{

}

void Sandbox::resume()
{

}

void Sandbox::setApplicationContext(ApplicationContext* context)
{
    m_appContext = context;
}

void Sandbox::accept(ISceneVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}

entt::registry& Sandbox::getRegistry()
{
    return m_reg;
}

SystemManager* Sandbox::getSystemManager()
{
    return &m_system;
}

void Sandbox::checkBoundary(const sf::Vector2u& boundary, sf::Sprite& obj)
{
    sf::Vector2f position = obj.getPosition();
    sf::FloatRect rect = obj.getGlobalBounds();

    if (position.x < 0)
        obj.setPosition(sf::Vector2f(0.f, position.y));

    if (position.x + (rect.size.x) > boundary.x)
        obj.setPosition(sf::Vector2f(boundary.x - (rect.size.x), position.y));

    if (position.y < 0)
        obj.setPosition(sf::Vector2f(position.x, 0.f));

    if (position.y + rect.size.y > boundary.y)
        obj.setPosition(sf::Vector2f(position.x, boundary.y - (rect.size.y)));
}