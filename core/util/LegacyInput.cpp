#include "LegacyInput.hpp"

#include <cctype>
#include <cinttypes>
#include <cstring>
#include <optional>
#include <string_view>

namespace
{
    struct ActionInfo
    {
        const char* name;
        const char* configKey;
        sf::Keyboard::Scancode defaultKey;
    };

    constexpr std::array<ActionInfo, legacyActionCount> actionInfo{{
        { "Up",    "key-up",    sf::Keyboard::Scancode::W },
        { "Left",  "key-left",  sf::Keyboard::Scancode::A },
        { "Down",  "key-down",  sf::Keyboard::Scancode::S },
        { "Right", "key-right", sf::Keyboard::Scancode::D },
        { "Jump",  "key-jump",  sf::Keyboard::Scancode::Space },
        { "Exit",  "key-exit",  sf::Keyboard::Scancode::Escape },
        { "Enter", "key-enter", sf::Keyboard::Scancode::Enter },
        { "Back",  "key-back",  sf::Keyboard::Scancode::Enter },
        { "A1",    "key-1",     sf::Keyboard::Scancode::U },
        { "A2",    "key-2",     sf::Keyboard::Scancode::I },
        { "A3",    "key-3",     sf::Keyboard::Scancode::O },
        { "A4",    "key-4",     sf::Keyboard::Scancode::J },
        { "A5",    "key-5",     sf::Keyboard::Scancode::K },
        { "A6",    "key-6",     sf::Keyboard::Scancode::L },
    }};

    // Key names used by config/config.json: single letters and digits, plus a few named keys.
    std::optional<sf::Keyboard::Scancode> scancodeFromName(std::string_view name)
    {
        if (name.size() == 1)
        {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
            if (c >= 'a' && c <= 'z')
                return static_cast<sf::Keyboard::Scancode>(static_cast<int>(sf::Keyboard::Scancode::A) + (c - 'a'));
            if (c >= '1' && c <= '9')
                return static_cast<sf::Keyboard::Scancode>(static_cast<int>(sf::Keyboard::Scancode::Num1) + (c - '1'));
            if (c == '0')
                return sf::Keyboard::Scancode::Num0;
        }
        if (name == "space") return sf::Keyboard::Scancode::Space;
        if (name == "esc" || name == "escape") return sf::Keyboard::Scancode::Escape;
        if (name == "enter" || name == "return") return sf::Keyboard::Scancode::Enter;
        if (name == "tab") return sf::Keyboard::Scancode::Tab;
        if (name == "backspace") return sf::Keyboard::Scancode::Backspace;
        if (name == "up") return sf::Keyboard::Scancode::Up;
        if (name == "down") return sf::Keyboard::Scancode::Down;
        if (name == "left") return sf::Keyboard::Scancode::Left;
        if (name == "right") return sf::Keyboard::Scancode::Right;
        return std::nullopt;
    }
}

const char* legacyActionName(LegacyAction action)
{
    const auto index = static_cast<std::size_t>(action);
    return index < legacyActionCount ? actionInfo[index].name : "?";
}

LegacyInputSampler::LegacyInputSampler()
    : m_bindings{}
    , m_eventDown{0}
    , m_pendingPressed{0}
    , m_pendingReleased{0}
{
    for (std::size_t i = 0; i < legacyActionCount; ++i)
        m_bindings[i] = actionInfo[i].defaultKey;
}

void LegacyInputSampler::loadBindings(const DataStore<>& config)
{
    for (std::size_t i = 0; i < legacyActionCount; ++i)
    {
        const auto name = config.get<std::string>(actionInfo[i].configKey);
        if (!name)
            continue;
        if (const auto key = scancodeFromName(*name))
            m_bindings[i] = *key;
    }
}

void LegacyInputSampler::onEvent(const sf::Event& event)
{
    if (const auto* keyPressed = event.getIf<sf::Event::KeyPressed>())
    {
        for (std::size_t i = 0; i < legacyActionCount; ++i)
        {
            const std::uint32_t bit = std::uint32_t{1} << i;
            if (m_bindings[i] == keyPressed->scancode && (m_eventDown & bit) == 0)
            {
                m_eventDown |= bit;
                m_pendingPressed |= bit;
            }
        }
    }
    else if (const auto* keyReleased = event.getIf<sf::Event::KeyReleased>())
    {
        for (std::size_t i = 0; i < legacyActionCount; ++i)
        {
            const std::uint32_t bit = std::uint32_t{1} << i;
            if (m_bindings[i] == keyReleased->scancode && (m_eventDown & bit) != 0)
            {
                m_eventDown &= ~bit;
                m_pendingReleased |= bit;
            }
        }
    }
    else if (event.is<sf::Event::FocusLost>())
    {
        // Releases may be delivered to another window; forget the event-side state.
        m_eventDown = 0;
    }
}

LegacyInputFrame LegacyInputSampler::sampleTick(std::uint64_t tick)
{
    LegacyInputFrame frame;
    frame.tick = tick;
    for (std::size_t i = 0; i < legacyActionCount; ++i)
    {
        if (sf::Keyboard::isKeyPressed(m_bindings[i]))
            frame.held |= std::uint32_t{1} << i;
    }
    frame.pressed = m_pendingPressed;
    frame.released = m_pendingReleased;
    m_pendingPressed = 0;
    m_pendingReleased = 0;
    return frame;
}

LegacyInputRecorder::~LegacyInputRecorder()
{
    close();
}

bool LegacyInputRecorder::open(const std::string& path, std::uint32_t rate)
{
    close();
    m_file = std::fopen(path.c_str(), "wb");
    if (!m_file)
        return false;

    std::fprintf(m_file, "# seinput v1 rate=%u\n# actions (bit 0 first):", rate);
    for (std::size_t i = 0; i < legacyActionCount; ++i)
        std::fprintf(m_file, " %s", actionInfo[i].name);
    std::fprintf(m_file, "\n# tick held pressed released (hex)\n");
    return true;
}

void LegacyInputRecorder::record(const LegacyInputFrame& frame)
{
    if (!m_file)
        return;
    std::fprintf(m_file, "%" PRIx64 " %" PRIx32 " %" PRIx32 " %" PRIx32 "\n",
        frame.tick, frame.held, frame.pressed, frame.released);
}

void LegacyInputRecorder::close()
{
    if (m_file)
    {
        std::fclose(m_file);
        m_file = nullptr;
    }
}

bool LegacyInputReader::open(const std::string& path, std::string* error)
{
    m_frames.clear();
    m_cursor = 0;
    m_held = 0;

    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
    {
        if (error) *error = "cannot open " + path;
        return false;
    }

    char line[256];
    int lineNumber = 0;
    bool ok = true;
    while (std::fgets(line, sizeof(line), file))
    {
        ++lineNumber;
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;

        std::uint64_t tick = 0;
        std::uint32_t held = 0, pressed = 0, released = 0;
        if (std::sscanf(p, "%" SCNx64 " %" SCNx32 " %" SCNx32 " %" SCNx32, &tick, &held, &pressed, &released) != 4
            || (!m_frames.empty() && tick <= m_frames.back().tick))
        {
            if (error) *error = path + ":" + std::to_string(lineNumber) + ": malformed or out-of-order line";
            ok = false;
            break;
        }
        m_frames.push_back(LegacyInputFrame{ tick, held, pressed, released });
    }
    std::fclose(file);
    if (!ok)
        m_frames.clear();
    return ok;
}

LegacyInputFrame LegacyInputReader::next(std::uint64_t tick)
{
    // Lines for ticks that were skipped still update the held state.
    while (m_cursor < m_frames.size() && m_frames[m_cursor].tick < tick)
        m_held = m_frames[m_cursor++].held;

    if (m_cursor < m_frames.size() && m_frames[m_cursor].tick == tick)
    {
        const LegacyInputFrame frame = m_frames[m_cursor++];
        m_held = frame.held;
        return frame;
    }

    return LegacyInputFrame{ tick, m_held, 0, 0 };
}
