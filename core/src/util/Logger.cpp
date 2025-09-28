#include "Logger.hpp"

Logger::Logger()
    : m_enableLogging(false)
    , m_severityLevel("trace")
{}

Logger& Logger::getInstance()
{
    static Logger instance;
    return instance;
}

boost::log::sources::severity_logger<boost::log::trivial::severity_level>& Logger::get()
{
    return Logger::getInstance().m_slg;
}

std::string Logger::getFileName()
{
    return Logger::getInstance().m_fileName;
}

void Logger::toggleLogging(bool option)
{
    m_enableLogging = option;
}

void Logger::setupConsoleLog()
{
    boost::log::add_console_log
    (
        std::cout,
        boost::log::keywords::format = boost::log::expressions::format("[%1%] %2%: [%3%:%4%] %5%")
        % boost::log::expressions::max_size_decor<char>(26)[boost::log::expressions::stream << std::setw(26) << boost::log::expressions::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S.%f")]
        % boost::log::expressions::max_size_decor<char>(7)[boost::log::expressions::stream << std::setw(7) << boost::log::trivial::severity]
        % boost::log::expressions::attr<boost::log::attributes::current_process_id::value_type>("ProcessID")
        % boost::log::expressions::attr<boost::log::attributes::current_thread_id::value_type>("ThreadID")
        % boost::log::expressions::smessage,
        boost::log::keywords::auto_flush = true
    );
    boost::log::add_common_attributes();
}

void Logger::setupFileLog(const std::string logPath)
{
    m_fileName = logPath + generateFilename();
    boost::log::add_file_log
    (
        m_fileName,
        boost::log::keywords::format = boost::log::expressions::format("[%1%] %2%: [%3%:%4%] %5%")
        % boost::log::expressions::max_size_decor<char>(26)[boost::log::expressions::stream << std::setw(26) << boost::log::expressions::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S.%f")]
        % boost::log::expressions::max_size_decor<char>(7)[boost::log::expressions::stream << std::setw(7) << boost::log::trivial::severity]
        % boost::log::expressions::attr<boost::log::attributes::current_process_id::value_type>("ProcessID")
        % boost::log::expressions::attr<boost::log::attributes::current_thread_id::value_type>("ThreadID")
        % boost::log::expressions::smessage,
        boost::log::keywords::auto_flush = true
    );
    boost::log::add_common_attributes();
}

void Logger::setFilterSeverity(std::string_view severityLevel)
{
    boost::log::core::get()->set_filter(boost::log::trivial::severity >= getFilterSeverity(severityLevel.data()));
}

void Logger::removeAllSinks()
{
    boost::log::core::get()->remove_all_sinks();
}

boost::log::trivial::severity_level Logger::getFilterSeverity(std::string_view severityLevel)
{
    static const std::unordered_map<std::string_view, boost::log::trivial::severity_level> m_filterMap =
    {
        { "trace", boost::log::trivial::trace },
        { "debug", boost::log::trivial::debug },
        { "info", boost::log::trivial::info },
        { "warning", boost::log::trivial::warning },
        { "error", boost::log::trivial::error },
        { "fatal", boost::log::trivial::fatal }
    };

    try
    {
        m_severityLevel = severityLevel;
        return m_filterMap.at(severityLevel);
    }
    catch (...)
    {
        m_severityLevel = "trace";
        return m_filterMap.at("trace");
    }
}

std::string Logger::generateFilename() const
{
    std::string filename = m_severityLevel + "_";
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::stringstream ss;
    boost::posix_time::time_facet* facet = new boost::posix_time::time_facet();
    facet->format("%Y-%m-%d_%H-%M-%S");
    ss.imbue(std::locale(std::locale::classic(), facet));
    ss << now;
    filename = filename + ss.str() + ".log";
    return filename;
}