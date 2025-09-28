#include "LogStream.hpp"

LogStream::LogStream()
    : m_lastReadPos(0)
{
}

void LogStream::open(const std::string& path)
{
    m_file.open(path, std::ios::in | std::ios::binary);
    if (!m_file.is_open())
        return;

    m_lines.clear();
    m_lastReadPos = 0;

    update();
}

void LogStream::update()
{
    if (!m_file.is_open())
        return;

    m_file.clear();
    m_file.seekg(m_lastReadPos);

    std::string line;
    while (std::getline(m_file, line))
    {
        LineInfo log;
        log.text = line;
        log.type = parseLineType(line);
        m_lines.push_back(std::move(log));
        m_lastReadPos = m_file.tellg();
    }

    // Trim the size of the history log if too big
    if (m_lines.size() > MAX_SIZE)
    {
        m_lines.erase(m_lines.begin(), m_lines.begin() + (m_lines.size() - MAX_SIZE));
    }
}

const std::vector<LogStream::LineInfo>& LogStream::getLog() const
{
    return m_lines;
}

LogStream::LogType LogStream::parseLineType(const std::string& line)
{
    // Example log: 
    // [2025-00-00 00:00:00.000000]    info: [0x00000000:0x00000000]                xxxx.cpp:00    | System initialize. Starting main thread...

    // Hardcoded - expect log lines with a type to be greater than 40 chars, anything else is not a log line
    if (line.size() < 40)
        return LogType::None;

    // There should not be a situation where the character after the bracket is the end... handle it anyways just in case
    size_t closeBracketPos = line.find(']');
    if (closeBracketPos == std::string_view::npos || closeBracketPos + 1 >= line.size())
        return LogType::None;

    // Move past ']' 
    size_t pos = closeBracketPos + 1;

    // Skip spaces
    while (pos < line.size() && line[pos] == ' ')
        ++pos;

    if (pos >= line.size())
        return LogType::None;

    // Check up to the largest LogType string size
    std::string_view level = std::string_view(line).substr(pos, 7);

    if (level.starts_with("trace"))
        return LogType::Trace;
    if (level.starts_with("debug"))
        return LogType::Debug;
    if (level.starts_with("info"))
        return LogType::Info;
    if (level.starts_with("warning"))
        return LogType::Warning;
    if (level.starts_with("error"))
        return LogType::Error;
    if (level.starts_with("fatal"))
        return LogType::Fatal;

    return LogType::None;
}

