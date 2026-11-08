#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <SFML/Window/Event.hpp>

#include "ApplicationContext.hpp"
#include "util/LegacyInput.hpp"

/**
 * @brief Command-line options of the legacy runtime.
*/
struct EngineOptions
{
    std::optional<std::uint64_t> seed;  // Overrides config "seed"
    std::string replayPath;             // .seinput whose frames replace the keyboard
    std::string recordPath;             // .seinput that records every tick's input frame
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

    // False when the configuration, the window or an input file could not be opened.
    bool ok() const;

private:
    bool openInputFiles();
    void configureWindow(std::unique_ptr<IScene> initialScene);
    void processEvents();
    void handleEvent(const sf::Event& event);
    void step();
    void renderFrame();

private:
    std::shared_ptr<ApplicationContext> m_appContext;
    EngineOptions m_options;
    bool m_windowActive;
    bool m_ok;
    LegacyInputSampler m_sampler;
    LegacyInputRecorder m_recorder;
    std::unique_ptr<LegacyInputReader> m_reader;
    LegacyInputFrame m_input;
};
