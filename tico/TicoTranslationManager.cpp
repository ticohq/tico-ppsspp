#include "TicoTranslationManager.h"

#include "tico/TicoConfig.h"

#include "dep/nlohmann/json.hpp"

#include <array>
#include <fstream>
#include <utility>

namespace Tico {
namespace {

using json = nlohmann::json;

constexpr const char *kDefaultLanguage = "English";

const std::pair<const char *, const char *> kEnglishFallback[] = {
	{"emulator_save_state", "Save State"},
	{"emulator_load_state", "Load State"},
	{"emulator_cheats", "Cheats"},
	{"emulator_settings", "Settings"},
	{"emulator_exit_game", "Exit Game"},
	{"emulator_slot", "Slot %d (%s)"},
	{"emulator_in_use", "In Use"},
	{"emulator_empty", "Empty"},
	{"emulator_enabled", "Enabled"},
	{"emulator_disabled", "Disabled"},
	{"emulator_cheat", "Cheat"},
	{"emulator_no_cheat_db", "No DB"},
	{"emulator_no_cheats", "No Cheats"},
	{"emulator_display_mode", "Display Mode"},
	{"emulator_size", "Size"},
	{"emulator_reset", "Reset"},
	{"emulator_integer", "Integer"},
	{"emulator_display", "Display"},
	{"emulator_select", "Select"},
	{"emulator_toggle", "Toggle"},
	{"emulator_back", "Back"},
	{"emulator_change", "Change"},
	{"emulator_auto", "Auto"},
	{"emulator_stretch", "Stretch"},
	{"emulator_original", "Original"},
	{"ra_title", "RetroAchievements"},
	{"ra_login_failed", "Login failed. Check your connection or credentials."},
	{"ra_missing_credentials", "Missing username/password or token."},
	{"ra_auth_failed", "Failed to authenticate."},
	{"ra_playing", "Playing: %s"},
	{"ra_game_identified", "Game identified."},
	{"ra_game_unsupported", "Rom hash doesn't match or game is unsupported."},
	{"ra_hash_support_disabled", "Hash support is not enabled in rcheevos."},
	{"ra_hardcore_mode", "Hardcore Mode"},
	{"ra_hardcore_savestate_disabled", "Save states are disabled in Hardcore Mode."},
	{"ra_game_mastered", "Game Mastered!"},
	{"ra_all_achievements_unlocked", "All achievements unlocked!"},
	{"ra_subset_completed", "Subset Completed"},
	{"ra_all_subset_achievements_unlocked", "All subset achievements unlocked."},
	{"ra_leaderboard_started", "Leaderboard Started"},
	{"ra_leaderboard_failed", "Leaderboard Failed"},
	{"ra_leaderboard", "Leaderboard"},
	{"ra_offline", "Offline. Unlocks will sync when reconnected."},
	{"ra_reconnected", "Reconnected."},
};

}  // namespace

TranslationManager &TranslationManager::Instance() {
	static TranslationManager instance;
	return instance;
}

bool TranslationManager::Init(LogCallback log) {
	if (log) {
		log_ = std::move(log);
	}

	const std::string language = ReadConfiguredLanguage();
	if (language == currentLanguage_ && !translations_.empty()) {
		return true;
	}

	currentLanguage_ = language;
	translations_.clear();
	const std::string fileName = LanguageFileName(language);
	if (!LoadLanguageFile(fileName)) {
		LogMessage(log_, "tico translations fallback language=%s file=%s", language.c_str(), fileName.c_str());
		LoadEnglishFallback();
		return false;
	}

	LogMessage(log_, "tico translations loaded language=%s file=%s count=%u",
		language.c_str(), fileName.c_str(), (unsigned)translations_.size());
	return true;
}

std::string TranslationManager::ReadConfiguredLanguage() const {
	const char *const configPaths[] = {
		"sdmc:/tico/config/general.jsonc",
		"sdmc:/tiicu/config/general.jsonc",
	};

	for (const char *path : configPaths) {
		std::ifstream file(path);
		if (!file.good()) {
			continue;
		}

		json config = json::parse(file, nullptr, false, true);
		if (!config.is_discarded() && config.contains("language") && config["language"].is_string()) {
			const std::string language = config["language"].get<std::string>();
			if (!language.empty()) {
				return language;
			}
		}
	}

	return kDefaultLanguage;
}

std::string TranslationManager::LanguageFileName(const std::string &language) const {
	if (language == "Portuguese" || language == "Portugues" || language == "pt") {
		return "pt.json";
	}
	if (language == "Espanol" || language == "Spanish" || language == "es") {
		return "es.json";
	}
	if (language == "Japanese" || language == "ja") {
		return "ja.json";
	}
	if (language == "French" || language == "fr") {
		return "fr.json";
	}
	if (language == "Chinese" || language == "Chinese Traditional" || language == "Chinese Simplified" || language == "zh") {
		return "zh.json";
	}
	return "en.json";
}

bool TranslationManager::LoadLanguageFile(const std::string &fileName) {
	const std::array<std::string, 3> paths = {{
		std::string(Paths::Lang) + "/" + fileName,
		std::string("sdmc:/tico/assets/lang/") + fileName,
		std::string("romfs:/lang/") + fileName,
	}};

	for (const std::string &path : paths) {
		std::ifstream file(path);
		if (!file.good()) {
			continue;
		}

		json parsed = json::parse(file, nullptr, false, true);
		if (parsed.is_discarded() || !parsed.is_object()) {
			LogMessage(log_, "tico translations parse failed path=%s", path.c_str());
			continue;
		}

		for (const auto &entry : parsed.items()) {
			if (entry.value().is_string()) {
				translations_[entry.key()] = entry.value().get<std::string>();
			}
		}
		return !translations_.empty();
	}

	return false;
}

void TranslationManager::LoadEnglishFallback() {
	translations_.clear();
	for (const auto &entry : kEnglishFallback) {
		translations_[entry.first] = entry.second;
	}
	currentLanguage_ = kDefaultLanguage;
}

std::string TranslationManager::GetString(const std::string &key) const {
	const auto it = translations_.find(key);
	return it == translations_.end() ? key : it->second;
}

std::string tr(const std::string &key) {
	return TranslationManager::Instance().GetString(key);
}

std::string tr(const char *key) {
	return key ? TranslationManager::Instance().GetString(key) : std::string();
}

}  // namespace Tico
