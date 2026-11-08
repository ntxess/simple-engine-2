#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "Engine.hpp"
#include "editor/Editor.hpp"
#include "serializer/JsonDataStoreSerializer.hpp"
#include "util/Logger.hpp"

namespace
{
	void printUsage()
	{
		std::fprintf(stderr, "usage: editor_app [--seed N] [--record FILE.seinput]\n");
	}

	// Parses the command line into options; returns false on an unknown or incomplete argument.
	bool parseArguments(int argc, char** argv, EngineOptions& options)
	{
		for (int i = 1; i < argc; ++i)
		{
			const std::string_view arg = argv[i];
			const bool hasValue = i + 1 < argc;
			if (arg == "--seed" && hasValue)
			{
				options.seed = std::strtoull(argv[++i], nullptr, 0);
			}
			else if (arg == "--record" && hasValue)
			{
				options.recordPath = argv[++i];
			}
			else
			{
				return false;
			}
		}
		return true;
	}
}

int main(int argc, char** argv)
{
	EngineOptions options;
	if (!parseArguments(argc, argv, options))
	{
		printUsage();
		return 2;
	}

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

	Engine engine("config/config.json", std::make_unique<Editor>(), options);
	if (!engine.ok())
	{
		LOG_FATAL(Logger::get()) << "Engine failed to start.";
		return -1;
	}
	engine.run();

	return 0;
}