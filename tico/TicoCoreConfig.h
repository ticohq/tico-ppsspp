#pragma once

#include <functional>
#include <map>
#include <string>

namespace Tico {

using LogCallback = std::function<void(const std::string &)>;

void LogMessage(const LogCallback &log, const char *fmt, ...);
void EnsureCoreConfigDirectories();

const std::string *FindOption(const std::map<std::string, std::string> &options, const char *key);
bool OptionEnabled(const std::string &value);
int OptionInt(const std::string &value, int fallback);
float OptionFloat(const std::string &value, float fallback);

class CoreConfig {
public:
	CoreConfig(std::string coreName, std::string configPath, std::string defaultJson, LogCallback log = {});

	void Load();
	void Save() const;
	void SetLogCallback(LogCallback log);

	bool IsLoaded() const { return loaded_; }
	const std::map<std::string, std::string> &Options() const { return options_; }
	std::string GetValue(const std::string &key, const std::string &fallback = "") const;
	void SetValue(const std::string &key, const std::string &value);

private:
	void CreateDefaultConfig() const;

	std::string coreName_;
	std::string configPath_;
	std::string defaultJson_;
	LogCallback log_;
	std::map<std::string, std::string> options_;
	bool loaded_ = false;
};

}  // namespace Tico
