#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ApplicationContext.hpp"

class Engine
{
public:
    Engine() = delete;
    Engine(const std::string& relativeConfigPath, std::unique_ptr<IScene> initialScene);

    void run();

private:
    void startThreads();
    void configureWindow(std::unique_ptr<IScene> initialScene);
    void physicThread();
    void renderThread();
    void audioThread();
    void resourceThread();

private:
    std::shared_ptr<ApplicationContext> m_appContext;
    std::atomic<bool> m_windowActive;
    std::thread m_physicThread;
    std::thread m_renderThread;
    std::thread m_audioThread;
    std::thread m_resourceThread;
    std::mutex m_mutex;
};