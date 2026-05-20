#include "tico/TicoRetroAchievements.h"

#include "tico/TicoAudioSfx.h"
#include "tico/TicoConfig.h"
#include "tico/TicoTranslationManager.h"

#include "Common/File/Path.h"
#include "Common/Net/HTTPRequest.h"
#include "Common/Render/ManagedTexture.h"
#include "Common/StringUtils.h"
#include "Core/Config.h"
#include "Core/MemMap.h"

#include "dep/nlohmann/json.hpp"
#include "rc_client.h"
#include "rc_error.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <utility>

namespace Tico {
namespace {

constexpr const char *kAccountsPath = "sdmc:/tico/config/accounts.jsonc";
constexpr const char *kBadgeCacheDir = "sdmc:/tico/assets/ra";
constexpr const char *kBadgeMediaBaseUrl = "http://media.retroachievements.org/Badge/";
constexpr uint32_t kPspConsoleId = 41;
constexpr uint32_t kPspMemoryOffset = 0x08000000;
constexpr float kStartupAlertDelaySeconds = 2.5f;

TicoRetroAchievements g_retroAchievements;
TicoRetroAchievements *g_activeRetroAchievements = nullptr;

void Log(const LogCallback &log, const char *fmt, ...) {
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

std::string SafeString(const char *value) {
	return value ? value : "";
}

std::string TrimCopy(std::string value) {
	auto isNotSpace = [](unsigned char c) { return !std::isspace(c); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), isNotSpace));
	value.erase(std::find_if(value.rbegin(), value.rend(), isNotSpace).base(), value.end());
	return value;
}

std::string LowerCopy(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return (char)std::tolower(c);
	});
	return value;
}

std::string ForcePlainHttp(std::string url) {
	constexpr const char *httpsPrefix = "https://";
	if (url.rfind(httpsPrefix, 0) == 0) {
		url.replace(0, std::strlen(httpsPrefix), "http://");
	}
	return url;
}

std::string TrFormat(const char *key, const char *value) {
	const std::string format = tr(key);
	return StringFromFormat(format.c_str(), value ? value : "");
}

bool FileExists(const std::string &path) {
	FILE *fp = fopen(path.c_str(), "rb");
	if (!fp) {
		return false;
	}
	fclose(fp);
	return true;
}

bool ReadFileBytes(const std::string &path, std::vector<uint8_t> *data) {
	data->clear();
	FILE *fp = fopen(path.c_str(), "rb");
	if (!fp) {
		return false;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return false;
	}
	const long size = ftell(fp);
	if (size <= 0) {
		fclose(fp);
		return false;
	}
	rewind(fp);
	data->resize((size_t)size);
	const size_t readSize = fread(data->data(), 1, data->size(), fp);
	fclose(fp);
	if (readSize != data->size()) {
		data->clear();
		return false;
	}
	return true;
}

std::string CleanBadgeName(const std::string &badgeName) {
	std::string clean;
	clean.reserve(badgeName.size());
	for (char c : badgeName) {
		const unsigned char uc = (unsigned char)c;
		if (std::isalnum(uc) || c == '_' || c == '-') {
			clean.push_back(c);
		}
	}
	return clean;
}

std::string BadgeCachePath(const std::string &badgeName) {
	return std::string(kBadgeCacheDir) + "/" + CleanBadgeName(badgeName) + ".png";
}

void EnsureRADirectories() {
	mkdir(Paths::Root, 0777);
	mkdir(Paths::Assets, 0777);
	mkdir("sdmc:/tico/config", 0777);
	mkdir(kBadgeCacheDir, 0777);
}

uint32_t RC_CCONV ReadMemory(uint32_t address, uint8_t *buffer, uint32_t numBytes, rc_client_t *) {
	const uint32_t pspAddress = address + kPspMemoryOffset;
	if (!Memory::IsValidRange(pspAddress, numBytes)) {
		return 0;
	}

	Memory::MemcpyUnchecked(buffer, pspAddress, numBytes);
	return numBytes;
}

void RC_CCONV ServerCall(const rc_api_request_t *request, rc_client_server_callback_t callback, void *callbackData, rc_client_t *) {
	if (!g_activeRetroAchievements || !request || !request->url || !callback) {
		if (callback) {
			rc_api_server_response_t response{};
			response.http_status_code = 0;
			callback(&response, callbackData);
		}
		return;
	}

	g_activeRetroAchievements->QueueServerCall(request->url, request->post_data, reinterpret_cast<void *>(callback), callbackData);
}

void RC_CCONV EventHandler(const rc_client_event_t *event, rc_client_t *) {
	if (g_activeRetroAchievements) {
		g_activeRetroAchievements->HandleEvent(event);
	}
}

void RC_CCONV LogMessage(const char *message, const rc_client_t *) {
	if (g_activeRetroAchievements) {
		g_activeRetroAchievements->LogRCMessage(message);
	}
}

}  // namespace

TicoRetroAchievements &RetroAchievements() {
	return g_retroAchievements;
}

void TicoRetroAchievements::Initialize(LogCallback log) {
	log_ = std::move(log);
	Shutdown();
	Log(log_, "RA: initialize");
	TranslationManager::Instance().Init(log_);

	LoadConfig();
	if (!enabled_) {
		Log(log_, "RA: disabled");
		return;
	}

	EnsureRADirectories();
	client_ = rc_client_create(ReadMemory, ServerCall);
	if (!client_) {
		Log(log_, "RA: rc_client_create failed");
		enabled_ = false;
		return;
	}

	initialized_ = true;
	++requestGeneration_;
	g_activeRetroAchievements = this;
	rc_client_enable_logging(client_, RC_CLIENT_LOG_LEVEL_INFO, LogMessage);
	rc_client_set_host(client_, "http://retroachievements.org");
	rc_client_set_event_handler(client_, EventHandler);
	rc_client_set_hardcore_enabled(client_, hardcore_ ? 1 : 0);

	if (!username_.empty() && !token_.empty()) {
		Log(log_, "RA: login with token user=%s", username_.c_str());
		rc_client_begin_login_with_token(client_, username_.c_str(), token_.c_str(),
			[](int result, const char *errorMessage, rc_client_t *, void *userdata) {
				auto *self = static_cast<TicoRetroAchievements *>(userdata);
				if (!self) {
					return;
				}
				if (result == RC_OK) {
					self->LogRCMessage("RA: token login succeeded");
					self->IdentifyGame();
				} else if (result == RC_INVALID_CREDENTIALS && !self->password_.empty()) {
					self->LogRCMessage("RA: token expired, trying password login");
					self->LoginWithPassword();
				} else {
					self->LogRCMessage(errorMessage ? errorMessage : "RA: token login failed");
					self->PushNotification(tr("ra_title"), tr("ra_login_failed"), "ra_icon");
				}
			},
			this);
	} else if (!username_.empty() && !password_.empty()) {
		Log(log_, "RA: login with password user=%s", username_.c_str());
		LoginWithPassword();
	} else {
		Log(log_, "RA: missing credentials in %s", kAccountsPath);
		PushNotification(tr("ra_title"), tr("ra_missing_credentials"), "ra_icon");
	}
}

void TicoRetroAchievements::LoadConfig() {
	enabled_ = false;
	hardcore_ = false;
	username_.clear();
	password_.clear();
	token_.clear();
	alertPosition_ = RAAlertPosition::TopRight;

	std::ifstream file(kAccountsPath);
	if (!file.is_open()) {
		Log(log_, "RA: accounts.jsonc not found at %s", kAccountsPath);
		return;
	}

	nlohmann::json json = nlohmann::json::parse(file, nullptr, false, true);
	if (json.is_discarded() || !json.is_object()) {
		Log(log_, "RA: accounts.jsonc parse failed");
		return;
	}

	enabled_ = json.value("ra_enabled", false);
	username_ = TrimCopy(json.value("ra_username", std::string()));
	password_ = json.value("ra_password", std::string());
	token_ = TrimCopy(json.value("ra_token", std::string()));
	hardcore_ = json.value("ra_hardcore_mode", false);

	const std::string position = LowerCopy(TrimCopy(json.value("ra_alert_position", std::string("top_right"))));
	if (position == "top_left") {
		alertPosition_ = RAAlertPosition::TopLeft;
	} else if (position == "bottom_left") {
		alertPosition_ = RAAlertPosition::BottomLeft;
	} else if (position == "bottom_right") {
		alertPosition_ = RAAlertPosition::BottomRight;
	} else {
		alertPosition_ = RAAlertPosition::TopRight;
	}

	Log(log_, "RA: config enabled=%d user=%s hardcore=%d", enabled_ ? 1 : 0, username_.c_str(), hardcore_ ? 1 : 0);
}

void TicoRetroAchievements::SaveToken(const std::string &token) {
	if (token.empty()) {
		return;
	}

	EnsureRADirectories();
	nlohmann::json json = nlohmann::json::object();
	std::ifstream in(kAccountsPath);
	if (in.is_open()) {
		nlohmann::json parsed = nlohmann::json::parse(in, nullptr, false, true);
		if (!parsed.is_discarded() && parsed.is_object()) {
			json = std::move(parsed);
		}
	}

	json["ra_enabled"] = enabled_;
	if (!username_.empty()) {
		json["ra_username"] = username_;
	}
	json["ra_token"] = token;
	json["ra_hardcore_mode"] = hardcore_;

	std::ofstream out(kAccountsPath, std::ios::binary);
	if (!out.good()) {
		Log(log_, "RA: failed writing token to %s", kAccountsPath);
		return;
	}
	out << json.dump(4) << "\n";
	token_ = token;
	Log(log_, "RA: token saved");
}

void TicoRetroAchievements::LoginWithPassword() {
	if (!client_ || username_.empty() || password_.empty()) {
		return;
	}

	rc_client_begin_login_with_password(client_, username_.c_str(), password_.c_str(),
		[](int result, const char *errorMessage, rc_client_t *client, void *userdata) {
			auto *self = static_cast<TicoRetroAchievements *>(userdata);
			if (!self) {
				return;
			}
			if (result == RC_OK) {
				self->LogRCMessage("RA: password login succeeded");
				const rc_client_user_t *user = rc_client_get_user_info(client);
				if (user && user->token) {
					self->SaveToken(user->token);
				}
				self->IdentifyGame();
			} else {
				self->LogRCMessage(errorMessage ? errorMessage : "RA: password login failed");
				self->PushNotification(tr("ra_title"), tr("ra_auth_failed"), "ra_icon");
			}
		},
		this);
}

void TicoRetroAchievements::SetGame(const std::string &path) {
	if (!path.empty()) {
		gamePath_ = path;
	}
	IdentifyGame();
}

void TicoRetroAchievements::IdentifyGame() {
	if (!enabled_ || !client_ || gamePath_.empty()) {
		return;
	}
	if (!rc_client_get_user_info(client_)) {
		Log(log_, "RA: waiting for login before identifying game");
		return;
	}
	if (identifying_) {
		return;
	}

#ifdef RC_CLIENT_SUPPORTS_HASH
	identifying_ = true;
	if (rc_client_is_game_loaded(client_)) {
		rc_client_unload_game(client_);
	}

	Log(log_, "RA: identifying PSP game path=%s", gamePath_.c_str());
	rc_client_begin_identify_and_load_game(client_, kPspConsoleId, gamePath_.c_str(), nullptr, 0,
		[](int result, const char *errorMessage, rc_client_t *client, void *userdata) {
			auto *self = static_cast<TicoRetroAchievements *>(userdata);
			if (!self) {
				return;
			}
			self->identifying_ = false;
			if (result == RC_OK) {
				const rc_client_game_t *game = rc_client_get_game_info(client);
				if (game && game->title) {
					self->PushNotification(tr("ra_title"), TrFormat("ra_playing", game->title), "ra_icon", kStartupAlertDelaySeconds);
				} else {
					self->PushNotification(tr("ra_title"), tr("ra_game_identified"), "ra_icon", kStartupAlertDelaySeconds);
				}
				self->PreloadBadges();
			} else {
				self->LogRCMessage(errorMessage ? errorMessage : "RA: game identification failed");
				self->PushNotification(tr("ra_title"), tr("ra_game_unsupported"), "ra_icon");
			}
		},
		this);
#else
	Log(log_, "RA: rcheevos was built without RC_CLIENT_SUPPORTS_HASH");
	PushNotification(tr("ra_title"), tr("ra_hash_support_disabled"), "ra_icon");
#endif
}

void TicoRetroAchievements::UnloadGame() {
	gamePath_.clear();
	identifying_ = false;
	if (client_ && rc_client_is_game_loaded(client_)) {
		rc_client_unload_game(client_);
	}
}

void TicoRetroAchievements::FrameUpdate() {
	if (!enabled_ || !client_ || identifying_) {
		return;
	}
	if (rc_client_is_game_loaded(client_) || rc_client_is_processing_required(client_)) {
		rc_client_do_frame(client_);
	}
}

void TicoRetroAchievements::Idle() {
	g_DownloadManager.Update();
	PumpBadgeDownloads();
	if (enabled_ && client_) {
		rc_client_idle(client_);
	}
}

void TicoRetroAchievements::Shutdown() {
	++requestGeneration_;
	UnloadGame();
	ReleaseBadgeTextures();
	notifications_.clear();
	badgeDownloads_.clear();
	badgeDownloadQueue_.clear();
	activeBadgeDownloads_ = 0;

	if (client_) {
		rc_client_destroy(client_);
		client_ = nullptr;
	}
	if (g_activeRetroAchievements == this) {
		g_activeRetroAchievements = nullptr;
	}

	initialized_ = false;
	identifying_ = false;
}

bool TicoRetroAchievements::HardcoreModeActive() const {
	return enabled_ && client_ && rc_client_get_hardcore_enabled(client_) != 0 && rc_client_get_user_info(client_) != nullptr;
}

bool TicoRetroAchievements::WarnIfHardcoreModeActive(bool isSaveStateAction) {
	if (!isSaveStateAction || !HardcoreModeActive()) {
		return false;
	}

	PushNotification(tr("ra_hardcore_mode"), tr("ra_hardcore_savestate_disabled"), "ra_icon");
	return true;
}

void TicoRetroAchievements::PreloadBadges() {
	if (!client_) {
		return;
	}

	rc_client_achievement_list_t *list = rc_client_create_achievement_list(client_,
		RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
	if (!list) {
		return;
	}

	for (uint32_t bucket = 0; bucket < list->num_buckets; ++bucket) {
		const rc_client_achievement_bucket_t &achievementBucket = list->buckets[bucket];
		for (uint32_t i = 0; i < achievementBucket.num_achievements; ++i) {
			const rc_client_achievement_t *achievement = achievementBucket.achievements[i];
			if (achievement && achievement->badge_name[0]) {
				DownloadBadge(achievement->badge_name);
			}
		}
	}

	rc_client_destroy_achievement_list(list);
}

void TicoRetroAchievements::PushNotification(const std::string &title, const std::string &description, const std::string &badgeName, float delaySeconds) {
	RANotification notification;
	notification.title = title;
	notification.description = description;
	notification.badgeName = badgeName.empty() ? "ra_icon" : badgeName;
	notification.timer = delaySeconds > 0.0f ? -delaySeconds : 0.0f;
	if (notifications_.size() >= 5) {
		notifications_.erase(notifications_.begin());
	}
	notifications_.push_back(std::move(notification));
}

void TicoRetroAchievements::DownloadBadge(const std::string &badgeName) {
	const std::string cleanBadgeName = CleanBadgeName(badgeName);
	if (cleanBadgeName.empty() || cleanBadgeName == "ra_icon") {
		return;
	}

	EnsureRADirectories();
	const std::string cachePath = BadgeCachePath(cleanBadgeName);
	if (FileExists(cachePath) || badgeDownloads_.find(cleanBadgeName) != badgeDownloads_.end()) {
		return;
	}

	badgeDownloads_.insert(cleanBadgeName);
	badgeDownloadQueue_.push_back(cleanBadgeName);
}

void TicoRetroAchievements::PumpBadgeDownloads() {
	constexpr int kMaxActiveBadgeDownloads = 1;
	if (activeBadgeDownloads_ >= kMaxActiveBadgeDownloads) {
		return;
	}

	while (activeBadgeDownloads_ < kMaxActiveBadgeDownloads && !badgeDownloadQueue_.empty()) {
		const std::string cleanBadgeName = badgeDownloadQueue_.front();
		badgeDownloadQueue_.erase(badgeDownloadQueue_.begin());
		const std::string cachePath = BadgeCachePath(cleanBadgeName);
		if (FileExists(cachePath)) {
			continue;
		}

		activeBadgeDownloads_++;
		const std::string url = std::string(kBadgeMediaBaseUrl) + cleanBadgeName + ".png";
		const uint32_t generation = requestGeneration_;
		g_DownloadManager.StartDownload(url, ::Path(cachePath), http::RequestFlags::Default, "image/png",
			"RetroAchievements badge",
			[this, generation, cleanBadgeName](http::Request &request) {
				if (activeBadgeDownloads_ > 0) {
					activeBadgeDownloads_--;
				}
				if (generation != requestGeneration_) {
					return;
				}
				if (request.ResultCode() != 200) {
					Log(log_, "RA: badge download failed badge=%s status=%d", cleanBadgeName.c_str(), request.ResultCode());
				}
			});
	}
}

Draw::Texture *TicoRetroAchievements::GetBadgeTexture(Draw::DrawContext *draw, const std::string &badgeName) {
	if (!draw) {
		return nullptr;
	}
	const std::string cleanBadgeName = CleanBadgeName(badgeName);
	if (cleanBadgeName.empty() || cleanBadgeName == "ra_icon") {
		return nullptr;
	}

	auto existing = badgeTextures_.find(cleanBadgeName);
	if (existing != badgeTextures_.end()) {
		return existing->second;
	}

	const std::string path = BadgeCachePath(cleanBadgeName);
	if (!FileExists(path)) {
		DownloadBadge(cleanBadgeName);
		return nullptr;
	}

	std::vector<uint8_t> data;
	if (!ReadFileBytes(path, &data)) {
		return nullptr;
	}

	Draw::Texture *texture = CreateTextureFromFileData(draw, data.data(), data.size(), ImageFileType::PNG, false, path.c_str());
	if (!texture) {
		return nullptr;
	}

	badgeTextures_[cleanBadgeName] = texture;
	return texture;
}

void TicoRetroAchievements::ReleaseBadgeTextures() {
	for (auto &entry : badgeTextures_) {
		if (entry.second) {
			entry.second->Release();
		}
	}
	badgeTextures_.clear();
}

void TicoRetroAchievements::QueueServerCall(const char *url, const char *postData, void *callback, void *callbackData) {
	if (!url || !callback) {
		return;
	}

	const std::string requestUrl = ForcePlainHttp(url);
	auto typedCallback = reinterpret_cast<rc_client_server_callback_t>(callback);
	const uint32_t generation = requestGeneration_;
	auto complete = [this, generation, typedCallback, callbackData](http::Request &request) {
		if (generation != requestGeneration_ || !client_) {
			return;
		}

		std::string body;
		request.buffer().TakeAll(&body);
		rc_api_server_response_t response{};
		response.body = body.c_str();
		response.body_length = body.size();
		response.http_status_code = request.ResultCode();
		typedCallback(&response, callbackData);
	};

	if (postData && postData[0]) {
		g_DownloadManager.AsyncPostWithCallback(requestUrl, postData, "application/x-www-form-urlencoded",
			http::RequestFlags::Default, std::move(complete), "RetroAchievements");
	} else {
		g_DownloadManager.StartDownload(requestUrl, ::Path(), http::RequestFlags::Default, nullptr,
			"RetroAchievements", std::move(complete));
	}
}

void TicoRetroAchievements::HandleEvent(const rc_client_event_t *event) {
	if (!event) {
		return;
	}

	switch (event->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		if (event->achievement) {
			TrophySfx().PlayTrophy();
			PushNotification(SafeString(event->achievement->title), SafeString(event->achievement->description),
				event->achievement->badge_name);
			DownloadBadge(event->achievement->badge_name);
		}
		break;
	case RC_CLIENT_EVENT_GAME_COMPLETED:
		TrophySfx().PlayTrophy();
		PushNotification(tr("ra_game_mastered"), tr("ra_all_achievements_unlocked"), "ra_icon");
		break;
	case RC_CLIENT_EVENT_SUBSET_COMPLETED:
		PushNotification(tr("ra_subset_completed"), event->subset && event->subset->title ? event->subset->title : tr("ra_all_subset_achievements_unlocked"), "ra_icon");
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
		if (event->leaderboard) {
			PushNotification(tr("ra_leaderboard_started"), SafeString(event->leaderboard->title), "ra_icon");
		}
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
		if (event->leaderboard) {
			PushNotification(tr("ra_leaderboard_failed"), SafeString(event->leaderboard->title), "ra_icon");
		}
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
		if (event->leaderboard) {
			PushNotification(tr("ra_leaderboard"), SafeString(event->leaderboard->title), "ra_icon");
		}
		break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
		if (event->achievement && event->achievement->measured_progress[0]) {
			PushNotification(SafeString(event->achievement->title), event->achievement->measured_progress,
				event->achievement->badge_name);
			DownloadBadge(event->achievement->badge_name);
		}
		break;
	case RC_CLIENT_EVENT_SERVER_ERROR:
		if (event->server_error) {
			PushNotification(tr("ra_title"), SafeString(event->server_error->error_message), "ra_icon");
		}
		break;
	case RC_CLIENT_EVENT_DISCONNECTED:
		PushNotification(tr("ra_title"), tr("ra_offline"), "ra_icon");
		break;
	case RC_CLIENT_EVENT_RECONNECTED:
		PushNotification(tr("ra_title"), tr("ra_reconnected"), "ra_icon");
		break;
	default:
		break;
	}
}

void TicoRetroAchievements::LogRCMessage(const char *message) {
	if (message && message[0]) {
		Log(log_, "%s", message);
	}
}

}  // namespace Tico
