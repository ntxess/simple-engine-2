#pragma once

#include <cstring>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/sources/severity_feature.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>

#if defined(_WIN32)  
#define __FILENAME__ (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)
#elif defined(__linux__)
#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
#endif

// NOTE: Use a single-statement, dangling-else-safe gate that still supports stream chaining.
#define	LOG_TRACE(logger) \
	for (bool _logOnce = Logger::isEnabled(); _logOnce; _logOnce = false) BOOST_LOG_SEV(logger, boost::log::trivial::severity_level::trace) << std::setw(25) << __FILENAME__ << ":" << std::setiosflags(std::ios::left) << std::setw(5) << __LINE__ << " | "
#define	LOG_DEBUG(logger) \
	for (bool _logOnce = Logger::isEnabled(); _logOnce; _logOnce = false) BOOST_LOG_SEV(logger, boost::log::trivial::severity_level::debug) << std::setw(25) << __FILENAME__ << ":" << std::setiosflags(std::ios::left) << std::setw(5) << __LINE__ << " | "
#define	LOG_INFO(logger) \
	for (bool _logOnce = Logger::isEnabled(); _logOnce; _logOnce = false) BOOST_LOG_SEV(logger, boost::log::trivial::severity_level::info) << std::setw(25) << __FILENAME__ << ":" << std::setiosflags(std::ios::left) << std::setw(5) << __LINE__ << " | "
#define	LOG_WARNING(logger) \
	for (bool _logOnce = Logger::isEnabled(); _logOnce; _logOnce = false) BOOST_LOG_SEV(logger, boost::log::trivial::severity_level::warning) << std::setw(25) << __FILENAME__ << ":" << std::setiosflags(std::ios::left) << std::setw(5) << __LINE__ << " | "
#define	LOG_ERROR(logger) \
	for (bool _logOnce = Logger::isEnabled(); _logOnce; _logOnce = false) BOOST_LOG_SEV(logger, boost::log::trivial::severity_level::error) << std::setw(25) << __FILENAME__ << ":" << std::setiosflags(std::ios::left) << std::setw(5) << __LINE__ << " | "
#define	LOG_FATAL(logger) \
	for (bool _logOnce = Logger::isEnabled(); _logOnce; _logOnce = false) BOOST_LOG_SEV(logger, boost::log::trivial::severity_level::fatal) << std::setw(25) << __FILENAME__ << ":" << std::setiosflags(std::ios::left) << std::setw(5) << __LINE__ << " | "

class Logger
{
public:
    static Logger& getInstance();
    static boost::log::sources::severity_logger<boost::log::trivial::severity_level>& get();
    static std::string getFileName();
    static bool isEnabled();
    void toggleLogging(bool option);
    void setupConsoleLog();
    void setupFileLog(const std::string logPath);
    void setFilterSeverity(std::string_view severityLevel);
    void removeAllSinks();

private:
    Logger();
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    boost::log::trivial::severity_level getFilterSeverity(std::string_view severityLevel);
    std::string generateFilename() const;

private:
    bool m_enableLogging;
    std::string m_severityLevel;
    boost::log::sources::severity_logger<boost::log::trivial::severity_level> m_slg;
    std::string m_fileName;
};