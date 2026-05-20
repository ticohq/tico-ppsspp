#pragma once

#include "tico/TicoCoreConfig.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Draw {
class DrawContext;
class Texture;
}

struct rc_client_t;
struct rc_client_event_t;

namespace Tico {

enum class RAAlertPosition {
	TopLeft = 0,
	TopRight,
	BottomLeft,
	BottomRight,
};

struct RANotification {
	std::string title;
	std::string description;
	std::string badgeName;
	float timer = 0.0f;
	float duration = 4.0f;
	float slideIn = 0.4f;
	float slideOut = 0.4f;
};

class TicoRetroAchievements {
public:
	void Initialize(LogCallback log = {});
	void SetGame(const std::string &path);
	void UnloadGame();
	void FrameUpdate();
	void Idle();
	void Shutdown();

	bool Enabled() const { return enabled_; }
	bool HardcoreModeActive() const;
	bool WarnIfHardcoreModeActive(bool isSaveStateAction);

	RAAlertPosition AlertPosition() const { return alertPosition_; }
	std::vector<RANotification> &Notifications() { return notifications_; }
	Draw::Texture *GetBadgeTexture(Draw::DrawContext *draw, const std::string &badgeName);
	void QueueServerCall(const char *url, const char *postData, void *callback, void *callbackData);
	void HandleEvent(const rc_client_event_t *event);
	void LogRCMessage(const char *message);

private:
	void LoadConfig();
	void SaveToken(const std::string &token);
	void LoginWithPassword();
	void IdentifyGame();
	void PreloadBadges();
	void PushNotification(const std::string &title, const std::string &description, const std::string &badgeName = "ra_icon", float delaySeconds = 0.0f);
	void DownloadBadge(const std::string &badgeName);
	void PumpBadgeDownloads();
	void ReleaseBadgeTextures();

	LogCallback log_;
	rc_client_t *client_ = nullptr;
	bool initialized_ = false;
	bool enabled_ = false;
	bool hardcore_ = false;
	bool identifying_ = false;
	std::string username_;
	std::string password_;
	std::string token_;
	std::string gamePath_;
	RAAlertPosition alertPosition_ = RAAlertPosition::TopRight;
	std::vector<RANotification> notifications_;
	std::map<std::string, Draw::Texture *> badgeTextures_;
	std::set<std::string> badgeDownloads_;
	std::vector<std::string> badgeDownloadQueue_;
	int activeBadgeDownloads_ = 0;
	uint32_t requestGeneration_ = 0;
};

TicoRetroAchievements &RetroAchievements();

}  // namespace Tico
