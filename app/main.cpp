#include <memory>

#include "Engine.hpp"
#include "editor/Editor.hpp"
#include "serializer/JsonDataStoreSerializer.hpp"
#include "util/Logger.hpp"

int main()
{
	// Start logging in the console until configuration file is read.
	Logger::getInstance().setupConsoleLog();
	LOG_INFO(Logger::get()) << "Engine pre-initialization setup.";

	// Temporarily create and read in configuraton file to setup file logger. 
	{
		JsonDataStoreSerializer configDataSerializer;
		auto configData = configDataSerializer.load("config/config.json");
		if (configData)
		{
			LOG_INFO(Logger::get()) << "Successfully read config file.";
			Logger::getInstance().toggleLogging(configData->get<bool>("debug-mode").value_or(false));
			Logger::getInstance().setFilterSeverity(configData->get<std::string>("debug-log-filter-severity").value_or("warning"));
			auto logPath = configData->get<std::string>("debug-log-folder").value_or("log/");
			LOG_INFO(Logger::get()) << "Now logging to file. Log file located at: " << logPath.c_str();
			Logger::getInstance().removeAllSinks();
			Logger::getInstance().setupFileLog(logPath.c_str());
		}
		else
		{
			LOG_FATAL(Logger::get()) << "Failed to read main config file. Aborting Engine initialization";
			return -1;
		}
	}

	Engine engine("config/config.json", std::make_unique<Editor>());
	engine.run();

	return 0;
}