#include "Sandbox.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

Sandbox::Sandbox()
    : m_appContext{nullptr}
    , m_player{entt::null}
    , m_bounds{0, 0}
{}

Sandbox::Sandbox(ApplicationContext* sysData)
    : m_appContext{sysData}
    , m_player{entt::null}
    , m_bounds{0, 0}
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
    assignSpawnIndex(m_reg, m_player);
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
    const std::uint32_t tickRate = m_appContext->simTime.rate;
    m_reg.emplace<UpdateEntityPolling>(m_player, msToTicks(1000, tickRate), true, m_appContext->simTime.tick);
    m_reg.emplace<EntityStatus>(m_player);
    m_reg.get<EntityStatus>(m_player).values["HP"] = 100.f;
    // Rule R5: the player walks at 120 px/s (the old 1 px per input poll, at 120 ticks per second).
    constexpr float playerSpeed = 120.f;
    const LegacySimTime& simTime = m_appContext->simTime;
    m_reg.get<PlayerInput>(m_player).input =
    {
        { LegacyAction::Up,    new Movement(m_player, { 0, -1 }, playerSpeed, simTime) },
        { LegacyAction::Left,  new Movement(m_player, { -1, 0 }, playerSpeed, simTime) },
        { LegacyAction::Down,  new Movement(m_player, { 0,  1 }, playerSpeed, simTime) },
        { LegacyAction::Right, new Movement(m_player, { 1,  0 }, playerSpeed, simTime) }
    };

    float width = static_cast<float>(m_appContext->configData.get<int>("width").value());
    float height = static_cast<float>(m_appContext->configData.get<int>("height").value());

    // The scene bounds come from the configured resolution, not from the window, so the
    // simulation does not depend on the OS window size.
    m_bounds = sf::Vector2u{ static_cast<unsigned int>(width), static_cast<unsigned int>(height) };

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

    // Rule R9: Input (processInput) -> WayPoint -> Boundary -> Collision -> Event -> GameRules ->
    // Cleanup (both at the end of update()). The systems run in the order they are added.
    m_system.addSystem<WayPointSystem>(true, "Speed");
    m_system.addSystem<BoundarySystem>(true, m_bounds);
    m_system.addSystem<CollisionSystem>(true, m_collisionEventReg, m_appContext->simTime, sf::Vector2f{ 0.f, 0.f }, m_bounds);
    m_system.addSystem<EventSystem>(true, m_collisionEventReg, m_appContext->simTime, msToTicks(36000, tickRate));
}

void Sandbox::processEvent(const sf::Event& event)
{
    // Keys are read per tick from ApplicationContext::input (processInput), so that a recorded
    // input file replays the scene exactly.
}

void Sandbox::processInput()
{
    const LegacyInputFrame* input = m_appContext->input;
    if (!input)
        return;

    // Exit (Escape) leaves for the main menu when the Sandbox runs as the root scene. Inside the
    // editor the Editor is the active scene and Escape stays an editor key, as before.
    if (input->wasPressed(LegacyAction::Exit) && m_appContext->sceneManager.getActiveScene() == this)
        m_appContext->sceneManager.addScene(std::make_unique<MainMenu>(m_appContext));

    // The player may have been destroyed (HP <= 0).
    if (!m_reg.valid(m_player) || !m_reg.all_of<PlayerInput>(m_player))
        return;

    m_reg.get<PlayerInput>(m_player).processInput(m_reg, *input);
}

void Sandbox::update()
{
    LOG_TRACE(Logger::get()) << "Entering update()";

    // Rule R5: systems take dt in seconds (WayPointSystem's Speed stat is in px/s).
    m_system.update(m_reg, static_cast<float>(m_appContext->simTime.dt));

    // GameRules: anything that has zero or less HP is requested for destruction (a missing HP stat
    // counts as 0)
    const auto& view = m_reg.view<EntityStatus>();
    for (const auto& entity : view)
    {
        if (m_reg.get<EntityStatus>(entity).getOr("HP", 0.f) <= 0.f)
            m_reg.emplace_or_replace<DestroyRequest>(entity);
    }

    // Cleanup, at the end of the tick (rule R10): destroy the requested entities in SpawnIndex order
    std::vector<std::pair<std::uint32_t, entt::entity>> doomed;
    for (const auto& entity : m_reg.view<DestroyRequest>())
    {
        const SpawnIndex* spawnIndex = m_reg.try_get<SpawnIndex>(entity);
        doomed.emplace_back(spawnIndex ? spawnIndex->value : UINT32_MAX, entity);
    }
    std::sort(doomed.begin(), doomed.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : entt::to_integral(a.second) < entt::to_integral(b.second);
    });
    for (const auto& [spawnIndex, entity] : doomed)
    {
        LOG_INFO(Logger::get()) << "Destroying entity [" << static_cast<unsigned int>(entity) << "]";

        m_system.getSystem<CollisionSystem>()->remove(m_reg, entity);
        m_reg.destroy(entity);
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

entt::entity Sandbox::getPlayer() const
{
    return m_player;
}

