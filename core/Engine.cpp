#include "Engine.hpp"

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
Engine::Engine(const std::string& relativeConfigPath, std::unique_ptr<IScene> initialScene)
    : m_appContext{std::make_shared<ApplicationContext>()}
    , m_windowActive{true}
{
    LOG_INFO(Logger::get()) << "Initializing system. Reading main configuration file...";

    auto configData = m_appContext->configDataSerializer.load(relativeConfigPath);
    if (configData)
    {
        LOG_DEBUG(Logger::get()) << "Loaded configuration data successfully.";
        m_appContext->configData = std::move(*configData);
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
    if (!m_appContext->window.setActive(false))
    {
        LOG_FATAL(Logger::get()) << "Failed to set Main thread to inactive";
        return;
    }

    startThreads();

    while (m_windowActive)
    {
        const std::optional<sf::Event> eventOpt = m_appContext->window.waitEvent();
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

    std::string name = m_appContext->configData.get<std::string>("name").value_or("Application");
    unsigned int width = m_appContext->configData.get<int>("width").value_or(1920);
    unsigned int height = m_appContext->configData.get<int>("height").value_or(1080);
    bool fullscreenEnabled = m_appContext->configData.get<bool>("fullscreen").value_or(false);
    m_appContext->aspectRatio = static_cast<float>(width) / static_cast<float>(height);
    m_appContext->deltaTime = static_cast<float>(1000.f / m_appContext->configData.get<double>("frame-rate").value_or(60.f));
    m_appContext->window.create(sf::VideoMode({width, height}), name, sf::Style::Default, (fullscreenEnabled ? sf::State::Fullscreen : sf::State::Windowed), settings);    
    m_appContext->window.setFramerateLimit(static_cast<unsigned int>(m_appContext->configData.get<double>("frame-rate").value_or(60.f)));
    m_appContext->sceneManager.addScene(std::move(initialScene), true, m_appContext.get());
    m_appContext->sceneManager.processChange();

    LOG_DEBUG(Logger::get()) << "Application Context: \n"
        << "\tWindow name: " << name << "\n"
        << "\tWindow width: " << width << "\n"
        << "\tWindow height: " << height << "\n"
        << "\tDelta time: " << m_appContext->deltaTime;
    
    LOG_DEBUG(Logger::get()) << "Configuration data: \n" << m_appContext->configData;
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
        while (m_windowActive && accumulator >= m_appContext->deltaTime)
        {
            std::unique_lock<std::mutex> guard(m_mutex);
            m_appContext->sceneManager.getActiveScene()->update();
            guard.unlock();
            accumulator -= m_appContext->deltaTime;
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

    if (!m_appContext->window.setActive(true))
        LOG_FATAL(Logger::get()) << "Failed to set Render thread to active";

    while (m_windowActive)
    {
        m_appContext->window.clear();
        m_appContext->sceneManager.processChange();
        std::unique_lock<std::mutex> guard(m_mutex);
        m_appContext->sceneManager.getActiveScene()->processInput();
        guard.unlock();
        m_appContext->sceneManager.getActiveScene()->render();
        m_appContext->window.display();
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