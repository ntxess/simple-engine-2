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
    , m_ok{false}
    , m_projectionEnabled{false}
    , m_maxTicks{0}
{
    LOG_INFO(Logger::get()) << "Initializing system. Reading main configuration file...";

    // Scenes read the tick's input through the context; it is a zero frame until the first tick.
    m_appContext->input = &m_input;

    auto configData = m_appContext->configDataSerializer.load(relativeConfigPath);
    if (configData)
    {
        LOG_DEBUG(Logger::get()) << "Loaded configuration data successfully.";
        m_appContext->configData = std::move(*configData);
        readSimulationSettings();
        m_ok = openFiles();
        if (m_ok)
        {
            configureWindow(std::move(initialScene));
            m_ok = m_appContext->window.isOpen();
        }
        m_windowActive = m_ok;
    }
    else
    {
        LOG_ERROR(Logger::get()) << "Failed to read main config file. Engine failed to start.";
    }

    LOG_INFO(Logger::get()) << "System initialize. Starting main loop...";
}

/**
 * @brief [Public] Runs the main loop until the window closes or maxTicks ticks have run. The
 * projection of tick 0 (the state after scene initialisation) is written first.
*/
void Engine::run()
{
    LOG_INFO(Logger::get()) << "----- Main loop started -----";

    if (m_windowActive && m_projectionEnabled)
        writeProjection();

    if (m_options.headless)
        runHeadless();
    else
        runLive();

    m_recorder.close();
    m_projection.close();

    LOG_INFO(Logger::get()) << "----- Main loop ended -----";
}

/**
 * @brief [Public] False when the engine cannot run: missing configuration, window, input or
 * output file.
*/
bool Engine::ok() const
{
    return m_ok;
}

ApplicationContext& Engine::context()
{
    return *m_appContext;
}

std::uint64_t Engine::ticksRun() const
{
    return m_appContext->simTime.tick;
}

std::uint64_t Engine::projectionHash() const
{
    return m_projection.runHash();
}

/**
 * @brief [Private] The paced loop. Each iteration polls the window events, runs the simulation
 * ticks that are due at LegacySimTime::rate (an integer accumulator in microseconds times the
 * rate, clamped to 0.25 s, at most maxStepsPerFrame ticks), then renders one frame.
*/
void Engine::runLive()
{
    using Clock = std::chrono::steady_clock;

    // One tick is 1/rate seconds, i.e. 1'000'000 units of (microseconds x rate).
    const std::uint64_t rate = m_appContext->simTime.rate;
    const std::uint64_t unitsPerTick = 1'000'000;
    const std::uint64_t maxUnits = maxAccumulatedMicroseconds * rate;
    std::uint64_t accumulator = 0;
    auto previous = Clock::now();

    while (m_windowActive && !reachedMaxTicks())
    {
        processEvents();
        if (!m_windowActive)
            break;

        const auto now = Clock::now();
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - previous).count();
        previous = now;
        // ImGui needs a positive frame time; the simulation never reads it.
        m_appContext->realFrameSeconds = std::max(static_cast<double>(elapsedUs) * 1e-6, 1e-6);

        accumulator = std::min(accumulator + static_cast<std::uint64_t>(std::max<long long>(elapsedUs, 0)) * rate, maxUnits);

        int steps = 0;
        while (m_windowActive && accumulator >= unitsPerTick && steps < maxStepsPerFrame && !reachedMaxTicks())
        {
            step();
            accumulator -= unitsPerTick;
            ++steps;
        }

        renderFrame();
    }

    if (m_windowActive)
    {
        m_windowActive = false;
        m_appContext->window.close();
    }
}

/**
 * @brief [Private] The headless loop: no wall clock. Without fpsOverride every frame runs one
 * tick; with it, frames advance a synthetic clock at fpsOverride Hz and run the ticks due, with
 * the same 0.25 s clamp and maxStepsPerFrame cap as the paced loop. Either way the simulation sees
 * the same ticks and inputs, only the number of rendered frames between them changes.
*/
void Engine::runHeadless()
{
    const std::uint64_t rate = m_appContext->simTime.rate;
    const std::uint64_t fps = m_options.fpsOverride;
    // Units of (ticks x fps): each frame adds `rate`, a tick costs `fps`.
    const std::uint64_t maxUnits = std::max<std::uint64_t>(rate * fps * maxAccumulatedMicroseconds / 1'000'000, fps);
    std::uint64_t accumulator = 0;
    m_appContext->realFrameSeconds = fps != 0 ? 1.0 / static_cast<double>(fps) : m_appContext->simTime.dt;

    while (m_windowActive && !reachedMaxTicks())
    {
        processEvents();
        if (!m_windowActive)
            break;

        std::uint64_t steps = 1;
        if (fps != 0)
        {
            accumulator = std::min(accumulator + rate, maxUnits);
            steps = std::min<std::uint64_t>(accumulator / fps, maxStepsPerFrame);
            accumulator -= steps * fps;
        }

        for (std::uint64_t i = 0; i < steps && m_windowActive && !reachedMaxTicks(); ++i)
            step();

        renderFrame();
    }

    if (m_windowActive)
    {
        m_windowActive = false;
        m_appContext->window.close();
    }
}

bool Engine::reachedMaxTicks() const
{
    // A headless run without a tick budget runs no ticks.
    if (m_maxTicks == 0)
        return m_options.headless;
    return m_appContext->simTime.tick >= m_maxTicks;
}

/**
 * @brief [Private] Reads the tick rate and seed; the scenes use both during init.
*/
void Engine::readSimulationSettings()
{
    const int tickRate = m_appContext->configData.get<int>("sim-tick-rate").value_or(120);
    m_appContext->simTime.setRate(static_cast<std::uint32_t>(std::max(tickRate, 1)));
    // Scenes seed their random streams from this during init (util/LegacyRandom.hpp).
    m_appContext->seed = m_options.seed.value_or(static_cast<std::uint64_t>(m_appContext->configData.get<int>("seed").value_or(0)));
}

/**
 * @brief [Private] Loads the key bindings and opens the replay, record and projection files.
*/
bool Engine::openFiles()
{
    m_sampler.loadBindings(m_appContext->configData);
    m_maxTicks = m_options.maxTicks;

    if (!m_options.replayPath.empty())
    {
        std::string error;
        m_reader = std::make_unique<LegacyInputReader>();
        if (!m_reader->open(m_options.replayPath, &error))
        {
            LOG_ERROR(Logger::get()) << "Failed to read input replay: " << error;
            return false;
        }
        if (m_maxTicks == 0)
            m_maxTicks = m_reader->lastTick();
    }

    if (!m_options.recordPath.empty() && !m_recorder.open(m_options.recordPath, m_appContext->simTime.rate))
    {
        LOG_ERROR(Logger::get()) << "Failed to open input recording: " << m_options.recordPath;
        return false;
    }

    m_projectionEnabled = m_options.computeProjection || !m_options.projectionPath.empty();
    if (!m_options.projectionPath.empty() && !m_projection.open(m_options.projectionPath, m_appContext->simTime.rate))
    {
        LOG_ERROR(Logger::get()) << "Failed to open projection output: " << m_options.projectionPath;
        return false;
    }

    return true;
}

/**
 * @brief [Private] Drains the window's event queue without blocking.
*/
void Engine::processEvents()
{
    while (const std::optional<sf::Event> eventOpt = m_appContext->window.pollEvent())
    {
        if (m_options.headless)
        {
            // Nothing reaches the scene or the input sampler of a headless run.
            if (eventOpt->is<sf::Event::Closed>())
            {
                m_windowActive = false;
                m_appContext->window.close();
                return;
            }
            continue;
        }

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
    // Key edges are latched for the next tick; a replay ignores the keyboard.
    if (!m_reader)
        m_sampler.onEvent(event);

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
 * @brief [Private] Runs one simulation tick: latch the tick's input frame (replayed, sampled from
 * the keyboard, or empty in a headless run without a replay), record it, run the scene, apply
 * scene changes at the tick boundary (so every tick runs entirely in one scene), then project the
 * resulting state.
*/
void Engine::step()
{
    const std::uint64_t tick = ++m_appContext->simTime.tick;
    if (m_reader)
        m_input = m_reader->next(tick);
    else if (m_options.headless)
        m_input = LegacyInputFrame{ tick, 0, 0, 0 };
    else
        m_input = m_sampler.sampleTick(tick);
    m_recorder.record(m_input);

    IScene* scene = m_appContext->sceneManager.getActiveScene();
    scene->processInput();
    scene->update();
    m_appContext->sceneManager.processChange();

    if (m_projectionEnabled)
        writeProjection();
}

void Engine::writeProjection()
{
    m_projection.write(m_appContext->simTime.tick, m_appContext->sceneManager.getActiveScene()->getRegistry());
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
    m_appContext->aspectRatio = static_cast<float>(width) / static_cast<float>(height);
    if (m_options.headless)
    {
        // A small hidden window: it only provides the OpenGL context that textures need. Scenes take
        // their bounds from the configured resolution, never from the window.
        m_appContext->window.create(sf::VideoMode({64, 64}), name, sf::Style::None, sf::State::Windowed, settings);
        m_appContext->window.setVisible(false);
        m_appContext->window.setFramerateLimit(0);
    }
    else
    {
        m_appContext->window.create(sf::VideoMode({width, height}), name, sf::Style::Default, (fullscreenEnabled ? sf::State::Fullscreen : sf::State::Windowed), settings);
        const double frameRate = m_options.fpsOverride != 0 ? static_cast<double>(m_options.fpsOverride) : m_appContext->configData.get<double>("frame-rate").value_or(60.f);
        m_appContext->window.setFramerateLimit(static_cast<unsigned int>(frameRate));
    }
    m_appContext->sceneManager.addScene(std::move(initialScene), true, m_appContext.get());
    m_appContext->sceneManager.processChange();

    LOG_DEBUG(Logger::get()) << "Application Context: \n"
        << "\tWindow name: " << name << "\n"
        << "\tWindow width: " << width << "\n"
        << "\tWindow height: " << height << "\n"
        << "\tSimulation tick rate: " << m_appContext->simTime.rate << "\n"
        << "\tSeed: " << m_appContext->seed << "\n"
        << "\tHeadless: " << m_options.headless;

    LOG_DEBUG(Logger::get()) << "Configuration data: \n" << m_appContext->configData;
}
