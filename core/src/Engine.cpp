#include "Engine.hpp"

/**
 * @brief [Public] Normal constuctor.
*/
Engine::Engine(const std::string& relativeConfigPath, std::unique_ptr<IScene> initialScene)
    : m_appContext(std::make_shared<ApplicationContext>())
    , m_windowActive(true)
{
    LOG_INFO(Logger::get()) << "Initializing system. Reading main configuration file...";

    if (m_appContext->configDataSerializer.load(relativeConfigPath, m_appContext->configData))
    {
        configureWindow(std::move(initialScene));
    }
    else
    {
        LOG_ERROR(Logger::get()) << "Failed to read main config file. Engine failed to start.";
        return;
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

    startThreads();

    sf::Event event;
    while (m_windowActive && m_appContext->window.waitEvent(event))
    {
        switch (event.type)
        {
        case sf::Event::Closed:
            LOG_INFO(Logger::get()) << "Triggered Event::Closed";
            m_windowActive = false;
            m_physicThread.join();
            m_renderThread.join();
            m_audioThread.join();
            m_resourceThread.join();
            m_appContext->window.close();
            break;

        case sf::Event::Resized:
        {
            LOG_INFO(Logger::get()) << "Triggered Event::Resized";
            float newWidth = static_cast<float>(event.size.width);
            float newHeight = newWidth / m_appContext->aspectRatio;
            if (newHeight > event.size.height)
            {
                newHeight = static_cast<float>(event.size.height);
                newWidth = newHeight * m_appContext->aspectRatio;
            }

            m_appContext->window.setSize(sf::Vector2u(
                static_cast<unsigned int>(newWidth),
                static_cast<unsigned int>(newHeight))
            );

            m_appContext->viewport.setSize(newWidth, newHeight);
            break;
        }

        case sf::Event::LostFocus:
            // Pause
            LOG_INFO(Logger::get()) << "Triggered Event::LostFocus";
            break;

        case sf::Event::GainedFocus:
            // Resume
            LOG_INFO(Logger::get()) << "Triggered Event::GainedFocus";
            break;

        default:
            m_appContext->sceneManager.getActiveScene()->processEvent(event);
            break;
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
    settings.antialiasingLevel = 0;
    settings.majorVersion = 4;
    settings.minorVersion = 3;

    std::string name = m_appContext->configData.get<std::string>("name").value_or("Application");
    unsigned int width = m_appContext->configData.get<int>("width").value_or(1920);
    unsigned int height = m_appContext->configData.get<int>("height").value_or(1080);
    m_appContext->aspectRatio = static_cast<float>(width) / static_cast<float>(height);
    m_appContext->deltaTime = static_cast<float>(1000.f / m_appContext->configData.get<double>("frame-rate").value_or(60.f));
    m_appContext->window.create(sf::VideoMode(width, height), name, sf::Style::Default, settings);
    m_appContext->window.setActive(false);
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
        // This has the benfit of maintaining a constant update tick but also catchup 
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

    m_appContext->window.setActive(true);

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