#pragma once

#include "tico/TicoCoreConfig.h"

#include <string>
#include <unordered_map>

namespace Tico {

class TranslationManager {
public:
	static TranslationManager &Instance();

	bool Init(LogCallback log = {});
	std::string GetString(const std::string &key) const;

	const std::string &CurrentLanguage() const { return currentLanguage_; }

private:
	TranslationManager() = default;

	std::string ReadConfiguredLanguage() const;
	std::string LanguageFileName(const std::string &language) const;
	bool LoadLanguageFile(const std::string &fileName);
	void LoadEnglishFallback();

	LogCallback log_;
	std::string currentLanguage_;
	std::unordered_map<std::string, std::string> translations_;
};

std::string tr(const std::string &key);
std::string tr(const char *key);

}  // namespace Tico
