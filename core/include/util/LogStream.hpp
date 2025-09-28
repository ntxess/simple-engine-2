#pragma once

#include <fstream>
#include <string>
#include <vector>

constexpr size_t MAX_SIZE = 500;

class LogStream
{
public:
    enum class LogType
    {
        None,
        Trace,
        Debug,
        Info,
        Warning,
        Error,
        Fatal
    };

    struct LineInfo
    {
        std::string text;
        LogType type;
    };

public:
    LogStream();

    void open(const std::string& path);
    void update();
    const std::vector<LogStream::LineInfo>& getLog() const;

private:
    LogType parseLineType(const std::string& line);

private:
    std::ifstream m_file;
    std::vector<LineInfo> m_lines;
    std::streampos m_lastReadPos;
};