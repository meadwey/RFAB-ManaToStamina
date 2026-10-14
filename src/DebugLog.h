#pragma once
#include <memory>
#include <string>
#include <vector>
#include "SKSE/Logger.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/msvc_sink.h"

namespace DebugLog
{
	inline void InitLogging(const std::string& a_name)
	{
		if (auto path = SKSE::log::log_directory()) {
			*path /= (a_name + ".log");

			auto sinks = std::vector<spdlog::sink_ptr>{
				std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true),
				std::make_shared<spdlog::sinks::msvc_sink_mt>()
			};

			auto logger = std::make_shared<spdlog::logger>(a_name, sinks.begin(), sinks.end());
			logger->set_level(spdlog::level::trace);
			logger->flush_on(spdlog::level::info);
			spdlog::set_default_logger(std::move(logger));
			spdlog::set_pattern("[%T.%e] [%=5t] [%L] %v");
		}
	}
}
