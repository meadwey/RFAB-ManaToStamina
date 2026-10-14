#pragma once
#include "SKSE/Logger.h"

namespace DebugLog
{
	inline void InitLogging(const std::string_view a_pluginName)
	{
		auto logsFolder = SKSE::log::log_directory();
		if (!logsFolder) {
			return;
		}
		const auto pluginFolder = *logsFolder / a_pluginName;
		std::filesystem::create_directories(pluginFolder);
		const auto logFile = pluginFolder / "RFABManaToStamina.log";
		SKSE::log::use_file(logFile);
		SKSE::log::set_level(SKSE::log::Level::kInfo);
		SKSE::log::flush_on(SKSE::log::Level::kWarn);
	}
}
