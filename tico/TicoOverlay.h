#pragma once

#include "Common/CommonTypes.h"
#include "tico/PpssppTicoConfig.h"
#include "tico/TicoConfig.h"
#include "tico/TicoCoreConfig.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

struct ImGuiContext;
struct ImDrawList;
struct ImVec2;

namespace Draw {
class DrawContext;
class Texture;
}

namespace Tico {

enum class OverlayAction {
	None,
	SaveState,
	LoadState,
	Reset,
	LoadCheats,
	ToggleCheat,
};

struct OverlayCommand {
	OverlayAction action = OverlayAction::None;
	int slot = 0;
};

enum class CheatMenuEntryKind {
	Cheat,
	Section,
	Tip,
};

struct CheatMenuEntry {
	std::string name;
	bool enabled = false;
	bool toggleable = true;
	int sourceIndex = -1;
	CheatMenuEntryKind kind = CheatMenuEntryKind::Cheat;
};

class Overlay {
public:
	bool Init(Draw::DrawContext *draw, const char *gamePath, LogCallback log = {});
	void Shutdown();

	bool HandleInput(u64 buttons, u64 pressed, int leftStickX, int leftStickY, int rightStickX, int rightStickY);
	void Render(Draw::DrawContext *draw);
	void SetSaveStateInfo(int currentSlot, const std::array<bool, Ppsspp::SaveStateSlotCount> &slotInUse);
	void SetCheatsEnabled(bool enabled);
	void SetCheatInfo(bool enabled, bool available, const std::vector<CheatMenuEntry> &entries);
	void ReloadDisplaySettings();

	bool IsReady() const { return ready_; }
	bool IsVisible() const { return visible_; }
	bool ShouldExitGame() const { return exitRequested_; }
	void ClearExitRequest() { exitRequested_ = false; }
	OverlayCommand ConsumeCommand();
	void SetVisible(bool visible);

private:
	enum class Menu {
		Quick,
		SaveStates,
		Cheats,
		Settings,
	};

	int ItemCount() const;
	int QuickMenuStorageIndex(int visibleIndex) const;
	void UpdateBattery(float deltaTime);
	void DrawUI(float width, float height, float deltaTime);
	void DrawBackground(::ImDrawList *drawList, ::ImVec2 displaySize, float ease);
	void DrawTitle(::ImDrawList *drawList, ::ImVec2 displaySize, float scale, float ease);
	void DrawMenu(::ImDrawList *drawList, ::ImVec2 displaySize, float scale, float ease);
	void DrawSocialArea(::ImDrawList *drawList, ::ImVec2 displaySize, float scale, float ease);
	void DrawHelpers(::ImDrawList *drawList, ::ImVec2 displaySize, float scale, float ease);
	void DrawStatus(::ImDrawList *drawList, ::ImVec2 displaySize, float scale, float ease, float deltaTime);
	void DrawRAAlerts(Draw::DrawContext *draw, ::ImDrawList *drawList, ::ImVec2 displaySize, float scale, float deltaTime);
	void CycleSetting(int direction);
	void ApplyDisplaySettings(bool save);
	void LoadSocial(Draw::DrawContext *draw);
	bool LoadAvatarTextureFromFile(Draw::DrawContext *draw, const char *path);
	bool LoadAvatarTextureFromMemory(Draw::DrawContext *draw, const unsigned char *data, size_t size, const char *tag);
	void ReleaseAvatarTexture();
	Draw::Texture *LoadRAIconTexture(Draw::DrawContext *draw);
	Draw::Texture *LoadLoaderTexture(Draw::DrawContext *draw);
	void ReleaseRAIconTexture();
	void ReleaseLoaderTexture();
	std::string TitleText() const;
	void ExecuteSelection();

	LogCallback log_;
	bool ready_ = false;
	bool visible_ = false;
	bool comboDown_ = false;
	bool exitRequested_ = false;
	int selection_ = 0;
	int settingsSelection_ = 0;
	Menu menu_ = Menu::Quick;
	OverlayAction saveStateMode_ = OverlayAction::SaveState;
	DisplaySettings displaySettings_;
	OverlayCommand pendingCommand_;
	int currentStateSlot_ = 0;
	std::array<bool, Ppsspp::SaveStateSlotCount> slotInUse_{};
	bool cheatsEnabled_ = false;
	bool cheatsAvailable_ = false;
	bool cheatsLoading_ = false;
	bool cheatsLoadCommandSent_ = false;
	int cheatsLoadingDelayFrames_ = 0;
	std::vector<CheatMenuEntry> cheats_;
	u64 lastAnalogNavMs_ = 0;
	u64 nextCheatVerticalNavMs_ = 0;
	u64 nextCheatHorizontalNavMs_ = 0;
	int cheatVerticalNavDir_ = 0;
	int cheatHorizontalNavDir_ = 0;
	float animTimer_ = 0.0f;
	float loaderTimer_ = 0.0f;
	std::string title_;
	std::string nickname_ = "Player 1";
	Draw::Texture *avatarTexture_ = nullptr;
	Draw::Texture *raIconTexture_ = nullptr;
	Draw::Texture *loaderTexture_ = nullptr;
	int avatarWidth_ = 0;
	int avatarHeight_ = 0;
	ImGuiContext *context_ = nullptr;
	bool psmReady_ = false;
	u32 batteryLevel_ = 100;
	bool charging_ = false;
	float batteryTimer_ = 0.0f;
};

}  // namespace Tico
