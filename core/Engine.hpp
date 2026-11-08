#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <SFML/Window/Event.hpp>

#include "ApplicationContext.hpp"

/**
 * @brief Command-line options of the legacy runtime.
*/
struct EngineOptions
{
    std::optional<std::uint64_t> seed;  // Overrides config "seed"
};

/**
 * @brief Single-threaded engine loop of the legacy reference.
 * One paced main-thread loop owns the window, the scenes and the simulation: events are polled,
 * then a fixed number of simulation ticks run at LegacySimTime::rate, then the frame is rendered.
 * Rendering never changes simulation state (docs/milestone-specs.md, M1a).
*/
class Engine
{
public:
    // At most this many ticks run per rendered frame; the accumulator is clamped to 0.25 s.
    static constexpr int maxStepsPerFrame = 8;
    static constexpr std::uint64_t maxAccumulatedMicroseconds = 250'000;

    Engine() = delete;
    Engine(const std::string& relativeConfigPath, std::unique_ptr<IScene> initialScene, EngineOptions options = {});

    void run();

private:
    void configureWindow(std::unique_ptr<IScene> initialScene);
    void processEvents();
    void handleEvent(const sf::Event& event);
    void step();
    void renderFrame();

private:
    std::shared_ptr<ApplicationContext> m_appContext;
    EngineOptions m_options;
    bool m_windowActive;
};
