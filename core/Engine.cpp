#include "Engine.hpp"

#include <algorithm>
#include <chrono>
#include <optional>

#include <SFML/Window/ContextSettings.hpp>
#include <SFML/Window/Event.hpp>
#include <SFML/Window/VideoMode.hpp>
#include <SFML/Window/WindowEnums.hpp>

#include "util/Logger.hpp"

/**
 * @brief [Public] Normal constuctor.
*/
Engine::Engine(const std::string& relativeConfigPath, std::unique_ptr<IScene> initialScene, EngineOptions options)
    : m_appContext{std::make_shared<ApplicationContext>()}
    , m_options{std::move(options)}
    , m_windowActive{false}
{
    LOG_INFO(Logger::get()) << "Initializing system. Reading main configuration file...";

    auto configData = m_appContext->configDataSerializer.load(relativeConfigPath);
    if (configData)
    {
        LOG_DEBUG(Logger::get()) << "Loaded configuration data successfully.";
        m_appContext->configData = std::move(*configData);
        configureWindow(std::move(initialScene));
        m_windowActive = m_appContext->window.isOpen();
    }
    else
    {
        LOG_ERROR(Logger::get()) << "Failed to read main config file. Engine failed to start.";
    }

    LOG_INFO(Logger::get()) << "System initialize. Starting main loop...";
}

/**
 * @brief [Public] The single main-thread loop. Each iteration polls the window events, runs the
 * simulation ticks that are due at LegacySimTime::rate (an integer accumulator in microseconds
 * times the rate, clamped to 0.25 s, at most maxStepsPerFrame ticks), then renders one frame.
*/
void Engine::run()
{
    LOG_INFO(Logger::get()) << "----- Main loop started -----";

    using Clock = std::chrono::steady_clock;

    // One tick is 1/rate seconds, i.e. 1'000'000 units of (microseconds x rate).
    const std::uint64_t rate = m_appContext->simTime.rate;
    const std::uint64_t unitsPerTick = 1'000'000;
    const std::uint64_t maxUnits = maxAccumulatedMicroseconds * rate;
    std::uint64_t accumulator = 0;
    auto previous = Clock::now();

    while (m_windowActive)
    {
        processEvents();
        if (!m_windowActive)
            break;

        const auto now = Clock::now();
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - previous).count();
        previous = now;

        accumulator = std::min(accumulator + static_cast<std::uint64_t>(std::max<long long>(elapsedUs, 0)) * rate, maxUnits);

        int steps = 0;
        while (m_windowActive && accumulator >= unitsPerTick && steps < maxStepsPerFrame)
        {
            step();
            accumulator -= unitsPerTick;
            ++steps;
        }

        renderFrame();
    }

    LOG_INFO(Logger::get()) << "----- Main loop ended -----";
}

/**
 * @brief [Private] Drains the window's event queue without blocking.
*/
void Engine::processEvents()
{
    while (const std::optional<sf::Event> eventOpt = m_appContext->window.pollEvent())
    {
        handleEvent(*eventOpt);
        if (!m_windowActive)
            return;
    }
}

/**
 * @brief [Private] Handles one window event: window management here, everything else goes to the
 * active scene.
*/
void Engine::handleEvent(const sf::Event& event)
{
    if (event.is<sf::Event::Closed>())
    {
        LOG_INFO(Logger::get()) << "Triggered Event::Closed";
        m_windowActive = false;
        m_appContext->window.close();
    }
    else if (const auto* resized = event.getIf<sf::Event::Resized>())
    {
        LOG_INFO(Logger::get()) << "Triggered Event::Resized";
        const unsigned int resizedWidth = static_cast<unsigned int>(resized->size.x);
        const unsigned int resizedHeight = static_cast<unsigned int>(resized->size.y);

        float newWidth = static_cast<float>(resizedWidth);
        float newHeight = newWidth / m_appContext->aspectRatio;
        if (newHeight > static_cast<float>(resizedHeight))
        {
            newHeight = static_cast<float>(resizedHeight);
            newWidth = newHeight * m_appContext->aspectRatio;
        }

        m_appContext->window.setSize(sf::Vector2u(
            static_cast<unsigned int>(newWidth),
            static_cast<unsigned int>(newHeight))
        );

        m_appContext->viewport.setSize({newWidth, newHeight});
    }
    else if (event.is<sf::Event::FocusLost>())
    {
        LOG_INFO(Logger::get()) << "Triggered Event::LostFocus";
        m_appContext->sceneManager.getActiveScene()->pause();
    }
    else if (event.is<sf::Event::FocusGained>())
    {
        LOG_INFO(Logger::get()) << "Triggered Event::GainedFocus";
        m_appContext->sceneManager.getActiveScene()->resume();
    }
    else
    {
        m_appContext->sceneManager.getActiveScene()->processEvent(event);
    }
}

/**
 * @brief [Private] Runs one simulation tick. Scene changes requested during the tick are applied
 * at its end, so every tick runs entirely in one scene.
*/
void Engine::step()
{
    ++m_appContext->simTime.tick;

    IScene* scene = m_appContext->sceneManager.getActiveScene();
    scene->processInput();
    scene->update();
    m_appContext->sceneManager.processChange();
}

/**
 * @brief [Private] Renders one frame of the active scene. Rendering must not change simulation state.
*/
void Engine::renderFrame()
{
    m_appContext->window.clear();
    m_appContext->sceneManager.getActiveScene()->render();
    m_appContext->window.display();
}

/**
 * @brief [Private] Setup the SFML window and opengl context.
*/
void Engine::configureWindow(std::unique_ptr<IScene> initialScene)
{
    LOG_INFO(Logger::get()) << "Configuring window...";

    sf::ContextSettings settings;
    settings.depthBits = 24;
    settings.stencilBits = 8;
    settings.antiAliasingLevel = 0;
    settings.majorVersion = 4;
    settings.minorVersion = 3;

    std::string name = m_appContext->configData.get<std::string>("name").value_or("Application");
    unsigned int width = m_appContext->configData.get<int>("width").value_or(1920);
    unsigned int height = m_appContext->configData.get<int>("height").value_or(1080);
    bool fullscreenEnabled = m_appContext->configData.get<bool>("fullscreen").value_or(false);
    const int tickRate = m_appContext->configData.get<int>("sim-tick-rate").value_or(120);
    m_appContext->simTime.setRate(static_cast<std::uint32_t>(std::max(tickRate, 1)));
    // Scenes seed their random streams from this during init (util/LegacyRandom.hpp).
    m_appContext->seed = m_options.seed.value_or(static_cast<std::uint64_t>(m_appContext->configData.get<int>("seed").value_or(0)));
    m_appContext->aspectRatio = static_cast<float>(width) / static_cast<float>(height);
    // Milliseconds per simulation tick (the scenes' systems still take dt in ms).
    m_appContext->deltaTime = static_cast<float>(m_appContext->simTime.dt * 1000.0);
    m_appContext->window.create(sf::VideoMode({width, height}), name, sf::Style::Default, (fullscreenEnabled ? sf::State::Fullscreen : sf::State::Windowed), settings);
    m_appContext->window.setFramerateLimit(static_cast<unsigned int>(m_appContext->configData.get<double>("frame-rate").value_or(60.f)));
    m_appContext->sceneManager.addScene(std::move(initialScene), true, m_appContext.get());
    m_appContext->sceneManager.processChange();

    LOG_DEBUG(Logger::get()) << "Application Context: \n"
        << "\tWindow name: " << name << "\n"
        << "\tWindow width: " << width << "\n"
        << "\tWindow height: " << height << "\n"
        << "\tSimulation tick rate: " << m_appContext->simTime.rate << "\n"
        << "\tSeed: " << m_appContext->seed << "\n"
        << "\tDelta time: " << m_appContext->deltaTime;

    LOG_DEBUG(Logger::get()) << "Configuration data: \n" << m_appContext->configData;
}
