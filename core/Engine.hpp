#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <SFML/Window/Event.hpp>

#include "ApplicationContext.hpp"
#include "util/LegacyInput.hpp"
#include "util/SemanticProjection.hpp"

/**
 * @brief Command-line options of the legacy runtime (editor_app and legacy_replay).
*/
struct EngineOptions
{
    std::optional<std::uint64_t> seed;  // Overrides config "seed"
    std::string replayPath;             // .seinput whose frames replace the keyboard
    std::string recordPath;             // .seinput that records every tick's input frame
    bool headless = false;              // Hidden window, no wall clock: ticks back to back, or at fpsOverride frames
    std::string projectionPath;         // .seproj that receives the semantic projection of every tick
    bool computeProjection = false;     // Hash the projection even without projectionPath
    std::uint64_t maxTicks = 0;         // Stop after this tick; 0 = the replay's last tick, or until closed (live)
    unsigned int fpsOverride = 0;       // Live: frame-rate limit; headless: synthetic frame rate
};

/**
 * @brief Single-threaded engine loop of the legacy reference.
 * One main-thread loop owns the window, the scenes and the simulation: events are polled,
 * then the simulation ticks that are due run at LegacySimTime::rate, then the frame is rendered.
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

    // False when the configuration, the window or an input or output file could not be opened.
    bool ok() const;
    ApplicationContext& context();
    std::uint64_t ticksRun() const;
    // Run hash of the semantic projection (util/SemanticProjection.hpp), over ticks 0..ticksRun().
    std::uint64_t projectionHash() const;
    // Frames rendered by run(); with the synthetic clock it shows the pacing (ticks due, 8-step cap).
    std::uint64_t framesRendered() const;

private:
    void readSimulationSettings();
    bool openFiles();
    void configureWindow(std::unique_ptr<IScene> initialScene);
    void runLive();
    void runHeadless();
    bool reachedMaxTicks() const;
    void processEvents();
    void handleEvent(const sf::Event& event);
    void step();
    void writeProjection();
    void renderFrame();

private:
    std::shared_ptr<ApplicationContext> m_appContext;
    EngineOptions m_options;
    bool m_windowActive;
    bool m_ok;
    bool m_projectionEnabled;
    std::uint64_t m_maxTicks;
    std::uint64_t m_framesRendered;
    LegacyInputSampler m_sampler;
    LegacyInputRecorder m_recorder;
    std::unique_ptr<LegacyInputReader> m_reader;
    LegacyInputFrame m_input;
    SemanticProjection m_projection;
};
