#include "TicoCoreConfig.h"

#include "dep/nlohmann/json.hpp"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <utility>

namespace Tico {

void LogMessage(const LogCallback &log, const char *fmt, ...) {
	if (!log || !fmt) {
		return;
	}

	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	log(buffer);
}

void EnsureCoreConfigDirectories() {
	mkdir("sdmc:/tico", 0777);
	mkdir("sdmc:/tico/config", 0777);
	mkdir("sdmc:/tico/config/cores", 0777);
}

const std::string *FindOption(const std::map<std::string, std::string> &options, const char *key) {
	auto it = options.find(key);
	return it == options.end() ? nullptr : &it->second;
}

bool OptionEnabled(const std::string &value) {
	std::string lower;
	lower.reserve(value.size());
	for (char ch : value) {
		lower.push_back((char)std::tolower((unsigned char)ch));
	}
	return lower != "disabled" && lower != "false" && lower != "0" && lower != "off" && lower != "no";
}

int OptionInt(const std::string &value, int fallback) {
	char *end = nullptr;
	const long parsed = std::strtol(value.c_str(), &end, 10);
	return end != value.c_str() ? (int)parsed : fallback;
}

float OptionFloat(const std::string &value, float fallback) {
	char *end = nullptr;
	const float parsed = std::strtof(value.c_str(), &end);
	return end != value.c_str() ? parsed : fallback;
}

CoreConfig::CoreConfig(std::string coreName, std::string configPath, std::string defaultJson, LogCallback log)
	: coreName_(std::move(coreName)), configPath_(std::move(configPath)), defaultJson_(std::move(defaultJson)), log_(std::move(log)) {
}

void CoreConfig::SetLogCallback(LogCallback log) {
	log_ = std::move(log);
}

void CoreConfig::CreateDefaultConfig() const {
	EnsureCoreConfigDirectories();
	std::ofstream out(configPath_);
	if (out.good()) {
		out << defaultJson_;
		LogMessage(log_, "tico %s config default created path=%s", coreName_.c_str(), configPath_.c_str());
	} else {
		LogMessage(log_, "tico %s config default create failed path=%s", coreName_.c_str(), configPath_.c_str());
	}
}

void CoreConfig::Load() {
	options_.clear();
	loaded_ = false;

	std::ifstream in(configPath_);
	if (!in.good()) {
		CreateDefaultConfig();
		in.open(configPath_);
	}

	nlohmann::json config;
	if (in.good()) {
		config = nlohmann::json::parse(in, nullptr, false, true);
	}
	if (config.is_discarded()) {
		LogMessage(log_, "tico %s config corrupt, recreating path=%s", coreName_.c_str(), configPath_.c_str());
		in.close();
		remove(configPath_.c_str());
		CreateDefaultConfig();
		std::ifstream retry(configPath_);
		if (retry.good()) {
			config = nlohmann::json::parse(retry, nullptr, false, true);
		}
	}

	if (config.is_object()) {
		for (const auto &entry : config.items()) {
			const nlohmann::json &value = entry.value();
			if (value.is_string()) {
				options_[entry.key()] = value.get<std::string>();
			} else if (value.is_boolean()) {
				options_[entry.key()] = value.get<bool>() ? "true" : "false";
			} else if (value.is_number_integer()) {
				options_[entry.key()] = std::to_string(value.get<int>());
			} else if (value.is_number_float()) {
				options_[entry.key()] = std::to_string(value.get<float>());
			}
		}
		loaded_ = true;
	}

	LogMessage(log_, "tico %s config loaded path=%s options=%u", coreName_.c_str(), configPath_.c_str(), (unsigned)options_.size());
}

void CoreConfig::Save() const {
	EnsureCoreConfigDirectories();

	nlohmann::json config = nlohmann::json::object();
	for (const auto &entry : options_) {
		config[entry.first] = entry.second;
	}

	std::ofstream out(configPath_);
	if (out.good()) {
		out << config.dump(4);
		LogMessage(log_, "tico %s config saved path=%s options=%u", coreName_.c_str(), configPath_.c_str(), (unsigned)options_.size());
	} else {
		LogMessage(log_, "tico %s config save failed path=%s", coreName_.c_str(), configPath_.c_str());
	}
}

std::string CoreConfig::GetValue(const std::string &key, const std::string &fallback) const {
	auto it = options_.find(key);
	return it == options_.end() ? fallback : it->second;
}

void CoreConfig::SetValue(const std::string &key, const std::string &value) {
	options_[key] = value;
}

}  // namespace Tico
