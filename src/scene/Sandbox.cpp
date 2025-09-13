#include "Sandbox.hpp"

Sandbox::Sandbox()
    : m_appContext(nullptr)
    , m_player(entt::null)
{}

Sandbox::Sandbox(ApplicationContext* sysData)
    : m_appContext(sysData)
    , m_player(entt::null)
{}

Sandbox::~Sandbox()
{
    m_reg.clear();
}

void Sandbox::init()
{
    DataStore texturePath;
    m_appContext->configDataSerializer.load("config/texture.json", texturePath);     // Load config file
    m_appContext->textureManager.load(texturePath, thor::Resources::Reuse); // Load the texture from loaded paths

    // Create the main player object
    m_player = m_reg.create();
    m_reg.emplace<Sprite>(m_player, m_appContext->textureManager["player"]);
    m_reg.emplace<TeamTag>(m_player, Team::FRIENDLY);
    m_reg.emplace<PlayerInput>(m_player);
    m_reg.emplace<EffectsList>(m_player);
    m_reg.emplace<UpdateEntityPolling>(m_player, std::chrono::milliseconds(1000), true);
    m_reg.emplace<EntityStatus>(m_player);
    m_reg.get<EntityStatus>(m_player).values["HP"] = 100.f;
    m_reg.get<PlayerInput>(m_player).input =
    {
        { sf::Keyboard::W, new Movement(m_player, { 0, -1 }) },
        { sf::Keyboard::A, new Movement(m_player, { -1, 0 }) },
        { sf::Keyboard::S, new Movement(m_player, { 0,  1 }) },
        { sf::Keyboard::D, new Movement(m_player, { 1,  0 }) }
    };

    float width = static_cast<float>(m_appContext->configData.get<int>("width").value());
    float height = static_cast<float>(m_appContext->configData.get<int>("height").value());

    m_reg.get<Sprite>(m_player).setPosition(0, 0);

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
    if (event.type == sf::Event::KeyPressed)
    {
        if (event.key.code == sf::Keyboard::Escape)
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
        if (m_reg.get<EntityStatus>(entity).values["HP"] <= 0)
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
                auto& sceneRenderTexture = m_reg.get<SceneViewRenderer>(sceneTextureID).rd;
                auto& spriteEntity = view.get<Sprite>(entity).sprite;
                checkBoundary(sceneRenderTexture.getSize(), spriteEntity);
                sceneRenderTexture.draw(view.get<Sprite>(entity).sprite);
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

    if (position.x + (rect.width) > boundary.x)
        obj.setPosition(sf::Vector2f(boundary.x - (rect.width), position.y));

    if (position.y < 0)
        obj.setPosition(sf::Vector2f(position.x, 0.f));

    if (position.y + rect.height > boundary.y)
        obj.setPosition(sf::Vector2f(position.x, boundary.y - (rect.height)));
}