#include "Engine.hpp"

#include <chrono>
#include <optional>
#include <thread>

#include <SFML/Window/ContextSettings.hpp>
#include <SFML/Window/Event.hpp>
#include <SFML/Window/VideoMode.hpp>
#include <SFML/Window/WindowEnums.hpp>

#include "util/Logger.hpp"

/**
 * @brief [Public] Normal constuctor.
*/
Engine::Engine(const std::string& relativeConfigPath, std::unique_ptr<IScene> initialScene)
    : m_appContext{}
    , m_windowActive{true}
{
    LOG_INFO(Logger::get()) << "Initializing system. Reading main configuration file...";

    auto configData = m_appContext.configDataSerializer.load(relativeConfigPath);
    if (configData)
    {
        LOG_DEBUG(Logger::get()) << "Loaded configuration data successfully.";
        m_appContext.configData = std::move(*configData);
        configureWindow(std::move(initialScene));
    }
    else
    {
        LOG_ERROR(Logger::get()) << "Failed to read main config file. Engine failed to start.";
    }

    LOG_INFO(Logger::get()) << "System initialize. Starting main thread...";
}

/**
 * @brief [Public] Main thread of the application. Responsible for handling opengl context and
 * SFML event handling.
*/
void Engine::run()
{
    LOG_INFO(Logger::get()) << "----- Main thread started -----";

    // Release the OpenGL context from the main thread before the render thread claims it.
    // This also ensures any resources created during init (textures, ImGui font atlas, etc.)
    // were created while the window context was active on this thread.
    if (!m_appContext.window.setActive(false))
    {
        LOG_FATAL(Logger::get()) << "Failed to set Main thread to inactive";
        return;
    }

    startThreads();

    while (m_windowActive)
    {
        const std::optional<sf::Event> eventOpt = m_appContext.window.waitEvent();
        if (!eventOpt)
        {
            LOG_INFO(Logger::get()) << "SF::Event::waitEvent() returned no event. Skipping event processing.";
            continue;
        }

        const sf::Event& event = *eventOpt;
        if (event.is<sf::Event::Closed>())
        {
            LOG_INFO(Logger::get()) << "Triggered Event::Closed";
            m_windowActive = false;
            if (m_physicThread.joinable()) m_physicThread.join();
            if (m_renderThread.joinable()) m_renderThread.join();
            if (m_audioThread.joinable()) m_audioThread.join();
            if (m_resourceThread.joinable()) m_resourceThread.join();
            m_appContext.window.close();
        }
        else if (const auto* resized = event.getIf<sf::Event::Resized>())
        {
            LOG_INFO(Logger::get()) << "Triggered Event::Resized";
            const unsigned int resizedWidth = static_cast<unsigned int>(resized->size.x);
            const unsigned int resizedHeight = static_cast<unsigned int>(resized->size.y);

            float newWidth = static_cast<float>(resizedWidth);
            float newHeight = newWidth / m_appContext.aspectRatio;
            if (newHeight > static_cast<float>(resizedHeight))
            {
                newHeight = static_cast<float>(resizedHeight);
                newWidth = newHeight * m_appContext.aspectRatio;
            }
    
            m_appContext.window.setSize(sf::Vector2u(
                static_cast<unsigned int>(newWidth),
                static_cast<unsigned int>(newHeight))
            );
    
            m_appContext.viewport.setSize({newWidth, newHeight});
        }
        else if (event.is<sf::Event::FocusLost>())
        {
            LOG_INFO(Logger::get()) << "Triggered Event::LostFocus";
            std::unique_lock<std::shared_mutex> lock(m_sceneMutex);
            m_appContext.sceneManager.getActiveScene()->pause();
        }
        else if (event.is<sf::Event::FocusGained>())
        {
            LOG_INFO(Logger::get()) << "Triggered Event::GainedFocus";
            std::unique_lock<std::shared_mutex> lock(m_sceneMutex);
            m_appContext.sceneManager.getActiveScene()->resume();
        }
        else
        {
            std::unique_lock<std::shared_mutex> lock(m_sceneMutex);
            m_appContext.sceneManager.getActiveScene()->processEvent(event);
        }
    }

    LOG_INFO(Logger::get()) << "----- Main thread ended -----";
}

void Engine::startThreads()
{
    m_physicThread = std::thread(&Engine::physicThread, this);
    m_renderThread = std::thread(&Engine::renderThread, this);
    m_audioThread = std::thread(&Engine::audioThread, this);
    m_resourceThread = std::thread(&Engine::resourceThread, this);
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

    std::string name = m_appContext.configData.getCoerced<std::string>("system.name").value_or("Application");
    unsigned int width = static_cast<unsigned int>(m_appContext.configData.getCoerced<int>("video-settings.resolution.width").value_or(1920));
    unsigned int height = static_cast<unsigned int>(m_appContext.configData.getCoerced<int>("video-settings.resolution.height").value_or(1080));
    bool fullscreenEnabled = m_appContext.configData.getCoerced<bool>("video-settings.fullscreen").value_or(false);
    m_appContext.aspectRatio = static_cast<float>(width) / static_cast<float>(height);
    m_appContext.deltaTime = static_cast<float>(1000.f / m_appContext.configData.getCoerced<double>("video-settings.frame-rate").value_or(60.0));
    m_appContext.window.create(sf::VideoMode({width, height}), name, sf::Style::Default, (fullscreenEnabled ? sf::State::Fullscreen : sf::State::Windowed), settings);    

    const bool vsyncEnabled = m_appContext.configData.getCoerced<bool>("video-settings.vsync").value_or(false);
    m_appContext.window.setVerticalSyncEnabled(vsyncEnabled);

    if (vsyncEnabled)
    {
        // When vsync is enabled, let presentation block naturally instead of an artificial limit.
        m_appContext.window.setFramerateLimit(0);
    }
    else
    {
        // If vsync is off, cap rendering rate (optional). Use config "frame-rate" for now.
        m_appContext.window.setFramerateLimit(static_cast<unsigned int>(m_appContext.configData.getCoerced<double>("video-settings.frame-rate").value_or(60.0)));
    }
    m_appContext.sceneManager.addScene(std::move(initialScene), true, &m_appContext);
    m_appContext.sceneManager.processChange();

    // Job system (used by jobified simulation). If config is missing, choose a conservative default.
    // 0 or missing => auto.
    const int cfgJobThreads = m_appContext.configData.getCoerced<int>("system.job-threads").value_or(0);
    const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    const size_t jobThreads =
        (cfgJobThreads > 0)
            ? static_cast<size_t>(cfgJobThreads)
            : static_cast<size_t>(std::max(1u, hw > 2 ? (hw - 2) : 1u));

    if (!m_appContext.jobScheduler)
        m_appContext.jobScheduler = std::make_unique<TaskScheduler>(jobThreads);
    m_appContext.jobThreadCount = jobThreads;

    LOG_DEBUG(Logger::get()) << "Application Context: \n"
        << "\tWindow name: " << name << "\n"
        << "\tWindow width: " << width << "\n"
        << "\tWindow height: " << height << "\n"
        << "\tDelta time: " << m_appContext.deltaTime << "\n"
        << "\tVSync: " << (vsyncEnabled ? "true" : "false") << "\n"
        << "\tJob threads: " << jobThreads;
    
    LOG_DEBUG(Logger::get()) << "Configuration data: \n" << m_appContext.configData;
}

/**
 * @brief [Private] Seperate thread for physics simulation.
*/
void Engine::physicThread()
{
    LOG_INFO(Logger::get()) << "----- Physic thread started -----";

    auto prevTime = std::chrono::steady_clock::now();
    double accumulator = 0.0f;

    while (m_windowActive)
    {
        auto currTime = std::chrono::steady_clock::now();
        auto elapsedTime = std::chrono::duration<double, std::milli>(currTime - prevTime).count();
        prevTime = currTime;
        accumulator += elapsedTime;

        // While the delay between frames is larger than the expected delta time, 
        // run the update() call until we catch up before the next render() call.
        // This has the benefit of maintaining a constant update tick but also catchup 
        // to real-time if the engine ever hits a major lag point.
        while (m_windowActive && accumulator >= m_appContext.deltaTime)
        {
            SE_FRAME_BEGIN(m_appContext.perf, "PhysicsThread");
            std::unique_lock<std::shared_mutex> guard(m_sceneMutex);
            {
                SE_COUNTER_SET(m_appContext.perf, "JobThreads", static_cast<std::int64_t>(m_appContext.jobThreadCount));
                SE_PROFILE_SCOPE(m_appContext.perf, "SceneManager::processChange");
                m_appContext.sceneManager.processChange();
            }
            {
                SE_PROFILE_SCOPE(m_appContext.perf, "Scene::processInput");
                m_appContext.sceneManager.getActiveScene()->processInput();
            }
            {
                SE_PROFILE_SCOPE(m_appContext.perf, "Scene::update");
                m_appContext.sceneManager.getActiveScene()->update();
            }
            {
                // Basic per-tick counters (cheap and extremely useful).
                auto& reg = m_appContext.sceneManager.getActiveScene()->getRegistry();
                SE_COUNTER_SET(
                    m_appContext.perf,
                    "EntitiesAlive",
                    static_cast<std::int64_t>(reg.storage<entt::entity>().size())
                );
            }
            guard.unlock();
            SE_FRAME_END(m_appContext.perf);
            accumulator -= m_appContext.deltaTime;
        }
    }

    LOG_INFO(Logger::get()) << "----- Physic thread ended -----";
}

/**
 * @brief [Private] Seperate thread for rendering.
*/
void Engine::renderThread()
{
    LOG_INFO(Logger::get()) << "----- Render thread started -----";

    if (!m_appContext.window.setActive(true))
        LOG_FATAL(Logger::get()) << "Failed to set Render thread to active";

    while (m_windowActive)
    {
        SE_FRAME_BEGIN(m_appContext.perf, "RenderThread");
        {
            SE_PROFILE_SCOPE(m_appContext.perf, "Frame");

            {
                SE_PROFILE_SCOPE(m_appContext.perf, "Window::clear");
                m_appContext.window.clear();
            }

            {
                SE_PROFILE_SCOPE(m_appContext.perf, "Scene::render");
                std::shared_lock<std::shared_mutex> guard(m_sceneMutex);
                m_appContext.sceneManager.getActiveScene()->render();
            }

            {
                SE_PROFILE_SCOPE(m_appContext.perf, "Window::display");
                m_appContext.window.display();
            }
        }
        SE_FRAME_END(m_appContext.perf);
    }

    LOG_INFO(Logger::get()) << "----- Render thread ended -----";
}

/**
 * @brief [Private] Seperate thread for audio management.
*/
void Engine::audioThread()
{
    LOG_INFO(Logger::get()) << "----- Audio thread started -----";

    //while (m_windowActive)
    //{

    //}

    LOG_INFO(Logger::get()) << "----- Audio thread ended -----";
}

/**
 * @brief [Private] Seperate thread for resource management.
*/
void Engine::resourceThread()
{
    LOG_INFO(Logger::get()) << "----- Resource thread started -----";

    //while (m_windowActive)
    //{

    //}

    LOG_INFO(Logger::get()) << "----- Resource thread ended -----";
}