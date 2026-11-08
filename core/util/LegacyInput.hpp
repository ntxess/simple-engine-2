#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <SFML/Window/Event.hpp>
#include <SFML/Window/Keyboard.hpp>

#include "util/DataStore.hpp"

/**
 * @brief Logical actions of the legacy reference, bound to keys by config/config.json
 * (control-settings.key-bindings). The bit index of an action in a LegacyInputFrame mask is its
 * enumerator value.
*/
enum class LegacyAction : std::uint8_t
{
    Up,
    Left,
    Down,
    Right,
    Jump,
    Exit,
    Enter,
    Back,
    A1,
    A2,
    A3,
    A4,
    A5,
    A6,
    Count
};

constexpr std::size_t legacyActionCount = static_cast<std::size_t>(LegacyAction::Count);

constexpr std::uint32_t actionBit(LegacyAction action)
{
    return std::uint32_t{1} << static_cast<std::uint32_t>(action);
}

/**
 * @brief The input one simulation tick consumes: the actions held when the tick was sampled and
 * the press and release edges seen since the previous tick.
*/
struct LegacyInputFrame
{
    std::uint64_t tick = 0;
    std::uint32_t held = 0;
    std::uint32_t pressed = 0;
    std::uint32_t released = 0;

    bool isHeld(LegacyAction action) const { return (held & actionBit(action)) != 0; }
    bool wasPressed(LegacyAction action) const { return (pressed & actionBit(action)) != 0; }
    bool wasReleased(LegacyAction action) const { return (released & actionBit(action)) != 0; }
};

/**
 * @brief Samples the keyboard into per-tick frames. Edges come from window events (key repeats
 * ignored); the held state is read from the keyboard once per tick.
*/
class LegacyInputSampler
{
public:
    LegacyInputSampler();

    // Reads key-up, key-left, ... key-6 from the flattened configuration; unknown names keep the
    // default binding (W, A, S, D, Space, Escape, Enter, Enter, U, I, O, J, K, L).
    void loadBindings(const DataStore<>& config);
    void onEvent(const sf::Event& event);
    LegacyInputFrame sampleTick(std::uint64_t tick);

private:
    std::array<sf::Keyboard::Scancode, legacyActionCount> m_bindings;
    std::uint32_t m_eventDown;
    std::uint32_t m_pendingPressed;
    std::uint32_t m_pendingReleased;
};

/**
 * @brief Writes frames as a .seinput file: a header comment, then one line per tick,
 * "tick held pressed released" in lower-case hex.
*/
class LegacyInputRecorder
{
public:
    LegacyInputRecorder() = default;
    ~LegacyInputRecorder();
    LegacyInputRecorder(const LegacyInputRecorder&) = delete;
    LegacyInputRecorder& operator=(const LegacyInputRecorder&) = delete;

    bool open(const std::string& path, std::uint32_t rate);
    bool isOpen() const { return m_file != nullptr; }
    void record(const LegacyInputFrame& frame);
    void close();

private:
    std::FILE* m_file = nullptr;
};

/**
 * @brief Reads a .seinput file. Lines must have strictly increasing ticks; a tick that has no line
 * repeats the previous held state with no edges (so hand-written files may be sparse).
*/
class LegacyInputReader
{
public:
    bool open(const std::string& path, std::string* error = nullptr);
    LegacyInputFrame next(std::uint64_t tick);
    std::uint64_t lastTick() const { return m_frames.empty() ? 0 : m_frames.back().tick; }

private:
    std::vector<LegacyInputFrame> m_frames;
    std::size_t m_cursor = 0;
    std::uint32_t m_held = 0;
};

const char* legacyActionName(LegacyAction action);
