#pragma once

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>

#include "ApplicationContext.hpp"
#include "interface/IRenderCommands2DEmitter.hpp"
#include "util/BatchRenderer2D.hpp"
#include "util/RenderCommands2D.hpp"

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
    // Single-owner engine-wide state/services. Scenes get a non-owning pointer to this.
    ApplicationContext m_appContext;
    std::atomic<bool> m_windowActive;
    std::thread m_physicThread;
    std::thread m_renderThread;
    std::thread m_audioThread;
    std::thread m_resourceThread;
    mutable std::shared_mutex m_sceneMutex;

    // Optional command-buffer renderer (used when the active scene implements IRenderCommands2DEmitter).
    TripleBufferedRenderCommands2D m_renderCmds2D;
    BatchRenderer2D m_batcher2D{ Batch2DConfig{ Batch2DBackend::VertexBuffer, Batch2DSortMode::PreserveOrder } };
    std::atomic<bool> m_useRenderCmds2D{false};
};