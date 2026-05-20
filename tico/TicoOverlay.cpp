#include "TicoOverlay.h"

#include "TicoConfig.h"
#include "TicoRetroAchievements.h"
#include "TicoTranslationManager.h"
#include "TicoUtils.h"
#include "Common/GPU/thin3d.h"
#include "Common/Math/lin/matrix4x4.h"
#include "Common/Render/ManagedTexture.h"
#include "Common/System/Display.h"
#include "ext/imgui/imgui.h"
#include "ext/imgui/imgui_impl_thin3d.h"
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "ext/nanosvg/src/nanosvg.h"
#include "ext/nanosvg/src/nanosvgrast.h"

#include <switch.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <vector>
#include <utility>

namespace Tico {
namespace {

constexpr float kOverlayAnimDuration = 0.4f;
constexpr float kQuickMenuWidth = 400.0f;

struct QuickMenuItem {
	const char *labelKey;
	enum class Action {
		SaveState,
		LoadState,
		Cheats,
		Settings,
		ExitGame,
	} action;
};

constexpr QuickMenuItem kQuickMenuItems[] = {
	{"emulator_save_state", QuickMenuItem::Action::SaveState},
	{"emulator_load_state", QuickMenuItem::Action::LoadState},
	{"emulator_cheats", QuickMenuItem::Action::Cheats},
	{"emulator_settings", QuickMenuItem::Action::Settings},
	{"emulator_exit_game", QuickMenuItem::Action::ExitGame},
};

constexpr int kAnalogNavThreshold = 16000;
constexpr u64 kAnalogNavRepeatMs = 180;
constexpr int kCheatAnalogNavThreshold = 18000;
constexpr u64 kCheatVerticalNavInitialRepeatMs = 280;
constexpr u64 kCheatVerticalNavRepeatMs = 105;
constexpr u64 kCheatHorizontalNavInitialRepeatMs = 320;
constexpr u64 kCheatHorizontalNavRepeatMs = 180;
constexpr int kCheatPageStep = 10;
constexpr float kCheatRowHeight = 44.0f;
constexpr float kCheatTipRowHeight = 26.0f;
constexpr float kCheatSectionTopMargin = 10.0f;
constexpr float kCheatTextScale = 0.76f;
constexpr float kCheatSectionTextScale = 0.70f;
constexpr float kCheatTipTextScale = 0.52f;

constexpr DisplaySize kAspectDisplaySizes[] = {
	DisplaySize::Stretch,
	DisplaySize::_4_3,
	DisplaySize::_16_9,
	DisplaySize::Original,
};

const char *const kCustomAvatarPaths[] = {
	"sdmc:/tico/assets/avatar.jpg",
	"sdmc:/tico/assets/avatar.jpeg",
	"sdmc:/tico/assets/avatar.png"
};

bool ReadFileBytes(const char *path, std::vector<unsigned char> *data) {
	data->clear();
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		return false;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return false;
	}
	const long fileSize = ftell(fp);
	if (fileSize <= 0) {
		fclose(fp);
		return false;
	}
	rewind(fp);

	data->resize((size_t)fileSize);
	const size_t readSize = fread(data->data(), 1, data->size(), fp);
	fclose(fp);
	if (readSize != data->size()) {
		data->clear();
		return false;
	}
	return true;
}

#ifdef __SWITCH__
bool FindAccountUid(AccountUid *uid) {
	if (R_SUCCEEDED(accountGetPreselectedUser(uid)) && accountUidIsValid(uid)) {
		return true;
	}
	if (R_SUCCEEDED(accountGetLastOpenedUser(uid)) && accountUidIsValid(uid)) {
		return true;
	}

	s32 userCount = 0;
	if (R_FAILED(accountGetUserCount(&userCount)) || userCount <= 0) {
		return false;
	}

	AccountUid uids[ACC_USER_LIST_SIZE];
	s32 actualTotal = 0;
	if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &actualTotal)) && actualTotal > 0) {
		*uid = uids[0];
		return accountUidIsValid(uid);
	}

	return false;
}
#endif

uint8_t *LoadImGuiFontData(const char *path, size_t *sizeOut) {
	*sizeOut = 0;
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		return nullptr;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return nullptr;
	}
	const long fileSize = ftell(fp);
	if (fileSize <= 0) {
		fclose(fp);
		return nullptr;
	}
	rewind(fp);

	uint8_t *data = (uint8_t *)ImGui::MemAlloc((size_t)fileSize);
	if (!data) {
		fclose(fp);
		return nullptr;
	}

	const size_t readSize = fread(data, 1, (size_t)fileSize, fp);
	fclose(fp);
	if (readSize != (size_t)fileSize) {
		ImGui::MemFree(data);
		return nullptr;
	}

	*sizeOut = (size_t)fileSize;
	return data;
}

uint8_t *LoadFirstImGuiFontData(const char *const *paths, size_t pathCount, size_t *sizeOut, const char **loadedPathOut) {
	for (size_t i = 0; i < pathCount; ++i) {
		uint8_t *data = LoadImGuiFontData(paths[i], sizeOut);
		if (data) {
			if (loadedPathOut) {
				*loadedPathOut = paths[i];
			}
			return data;
		}
	}

	if (loadedPathOut) {
		*loadedPathOut = nullptr;
	}
	*sizeOut = 0;
	return nullptr;
}

float EaseOutCubic(float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	return 1.0f - std::pow(1.0f - t, 3.0f);
}

void DrawOverlayText(ImDrawList *drawList, ImFont *font, float fontSize, ImVec2 pos, ImU32 color, const std::string &text) {
	const char *begin = text.c_str();
	const char *end = begin + text.size();
	drawList->AddText(font, fontSize, ImVec2(pos.x + 1.5f, pos.y + 1.5f), IM_COL32(0, 0, 0, 50), begin, end);
	drawList->AddText(font, fontSize, pos, color, begin, end);
}

void DrawSwitchButtonPrompt(ImDrawList *drawList, ImFont *font, float fontSize, ImVec2 center, float size, const char *symbol, float alpha) {
	const ImU32 fillColor = IM_COL32(220, 220, 220, (int)(255.0f * alpha));
	const ImU32 textColor = IM_COL32(40, 40, 40, (int)(255.0f * alpha));
	drawList->AddCircleFilled(center, size * 0.5f, fillColor, 12);

	const float symbolSize = fontSize * 0.75f;
	const ImVec2 textSize = font->CalcTextSizeA(symbolSize, 10000.0f, 0.0f, symbol);
	drawList->AddText(font, symbolSize, ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f), textColor, symbol);
}

void DrawRotatingImage(ImDrawList *drawList, ImTextureID textureId, ImVec2 center, float size, float angle, ImU32 color) {
	const float half = size * 0.5f;
	const float cosA = std::cos(angle);
	const float sinA = std::sin(angle);
	const auto rotate = [&](float x, float y) {
		return ImVec2(center.x + x * cosA - y * sinA, center.y + x * sinA + y * cosA);
	};
	drawList->AddImageQuad(textureId,
		rotate(-half, -half),
		rotate(half, -half),
		rotate(half, half),
		rotate(-half, half),
		ImVec2(0.0f, 0.0f),
		ImVec2(1.0f, 0.0f),
		ImVec2(1.0f, 1.0f),
		ImVec2(0.0f, 1.0f),
		color);
}

void DrawFallbackSpinner(ImDrawList *drawList, ImVec2 center, float radius, float angle, float scale, float alpha) {
	for (int i = 0; i < 8; ++i) {
		const float t = ((float)i / 8.0f) * 6.2831853f + angle;
		const float lineAlpha = alpha * (0.25f + 0.75f * ((float)i / 7.0f));
		const ImVec2 p0(center.x + std::cos(t) * radius * 0.45f, center.y + std::sin(t) * radius * 0.45f);
		const ImVec2 p1(center.x + std::cos(t) * radius, center.y + std::sin(t) * radius);
		drawList->AddLine(p0, p1, IM_COL32(235, 235, 235, (int)(255.0f * lineAlpha)), 2.0f * scale);
	}
}

DisplaySize CycleDisplaySize(DisplaySize current, const DisplaySize *sizes, int count, int direction) {
	int index = 0;
	for (int i = 0; i < count; ++i) {
		if (sizes[i] == current) {
			index = i;
			break;
		}
	}

	index = (index + direction) % count;
	if (index < 0) {
		index += count;
	}
	return sizes[index];
}

int GetAvailableIntegerDisplaySizes(DisplaySize *sizes, int sizeCount) {
	if (!sizes || sizeCount <= 0) {
		return 0;
	}

	int count = 0;
	const int maxScale = MaxPpssppIntegerScaleForCurrentDisplay();
	auto addSize = [&](DisplaySize size) {
		if (count < sizeCount) {
			sizes[count++] = size;
		}
	};

	if (maxScale >= 1) addSize(DisplaySize::_1x);
	if (maxScale >= 2) addSize(DisplaySize::_2x);
	if (maxScale >= 3) addSize(DisplaySize::_3x);
	if (maxScale >= 4) addSize(DisplaySize::_4x);
	addSize(DisplaySize::Auto);
	return count;
}

std::string TranslatedDisplayModeLabel(DisplayMode mode) {
	return mode == DisplayMode::Integer ? tr("emulator_integer") : tr("emulator_display");
}

std::string TranslatedDisplaySizeLabel(DisplaySize size) {
	switch (size) {
	case DisplaySize::Stretch: return tr("emulator_stretch");
	case DisplaySize::_4_3: return "4:3";
	case DisplaySize::_16_9: return "16:9";
	case DisplaySize::Original: return tr("emulator_original");
	case DisplaySize::_1x: return "1x";
	case DisplaySize::_2x: return "2x";
	case DisplaySize::_3x: return "3x";
	case DisplaySize::_4x: return "4x";
	case DisplaySize::Auto: return tr("emulator_auto");
	default: return DisplaySizeLabel(size);
	}
}

std::string TruncateToWidth(ImFont *font, float fontSize, const std::string &text, float maxWidth) {
	if (!font || text.empty() || font->CalcTextSizeA(fontSize, 10000.0f, 0.0f, text.c_str()).x <= maxWidth) {
		return text;
	}

	std::string result = text;
	while (result.size() > 4) {
		result.resize(result.size() - 2);
		const std::string candidate = result + "...";
		if (font->CalcTextSizeA(fontSize, 10000.0f, 0.0f, candidate.c_str()).x <= maxWidth) {
			return candidate;
		}
	}
	return "...";
}

u64 CurrentTimeMs() {
	const u64 tickFreq = armGetSystemTickFreq();
	const u64 ticksPerMs = tickFreq / 1000;
	return ticksPerMs > 0 ? armGetSystemTick() / ticksPerMs : 0;
}

bool HeldNavigationTriggered(int heldDir, int &activeDir, u64 &nextRepeatMs, u64 initialRepeatMs, u64 repeatMs, u64 nowMs) {
	if (heldDir == 0) {
		activeDir = 0;
		nextRepeatMs = 0;
		return false;
	}
	if (heldDir != activeDir) {
		activeDir = heldDir;
		nextRepeatMs = nowMs + initialRepeatMs;
		return true;
	}
	if (nowMs >= nextRepeatMs) {
		nextRepeatMs = nowMs + repeatMs;
		return true;
	}
	return false;
}

bool IsSelectableCheatRow(const std::vector<CheatMenuEntry> &cheats, int index) {
	return index >= 0 && index < (int)cheats.size() && cheats[index].toggleable;
}

int FirstSelectableCheatRow(const std::vector<CheatMenuEntry> &cheats) {
	for (int i = 0; i < (int)cheats.size(); ++i) {
		if (IsSelectableCheatRow(cheats, i)) {
			return i;
		}
	}
	return 0;
}

int FindNextSelectableCheatRow(const std::vector<CheatMenuEntry> &cheats, int selection, int direction) {
	const int count = (int)cheats.size();
	if (count <= 0 || direction == 0) {
		return 0;
	}

	int index = std::clamp(selection, 0, count - 1);
	for (int attempts = 0; attempts < count; ++attempts) {
		index = (index + direction + count) % count;
		if (IsSelectableCheatRow(cheats, index)) {
			return index;
		}
	}
	return std::clamp(selection, 0, count - 1);
}

void MoveCheatSelectionWrapped(int &selection, const std::vector<CheatMenuEntry> &cheats, int delta) {
	if (delta == 0) {
		return;
	}
	if (cheats.empty()) {
		selection = 0;
		return;
	}

	if (!IsSelectableCheatRow(cheats, selection)) {
		selection = FindNextSelectableCheatRow(cheats, selection, delta > 0 ? 1 : -1);
		if (IsSelectableCheatRow(cheats, selection)) {
			return;
		}
	}

	const int direction = delta > 0 ? 1 : -1;
	const int steps = std::abs(delta);
	for (int i = 0; i < steps; ++i) {
		const int next = FindNextSelectableCheatRow(cheats, selection, direction);
		if (next == selection) {
			break;
		}
		selection = next;
	}
}

float CheatRowHeight(const std::vector<CheatMenuEntry> &cheats, int index, float scale) {
	if (index >= 0 && index < (int)cheats.size() && cheats[index].kind == CheatMenuEntryKind::Tip) {
		return kCheatTipRowHeight * scale;
	}
	if (index >= 0 && index < (int)cheats.size() && cheats[index].kind == CheatMenuEntryKind::Section) {
		return (kCheatRowHeight + kCheatSectionTopMargin) * scale;
	}
	return kCheatRowHeight * scale;
}

float CheatRowTopPadding(const std::vector<CheatMenuEntry> &cheats, int index, float scale) {
	if (index >= 0 && index < (int)cheats.size() && cheats[index].kind == CheatMenuEntryKind::Section) {
		return kCheatSectionTopMargin * scale;
	}
	return 0.0f;
}

void CalculateVisibleCheatRows(const std::vector<CheatMenuEntry> &cheats, int totalItemCount, int selection, float displayHeight, float scale,
		int *firstItem, int *visibleItemCount, float *contentHeight) {
	*firstItem = 0;
	*visibleItemCount = totalItemCount;
	*contentHeight = 0.0f;
	if (totalItemCount <= 0) {
		return;
	}

	const float maxMenuHeight = std::clamp(displayHeight * 0.62f, 6.0f * kCheatRowHeight * scale, 12.0f * kCheatRowHeight * scale);
	const int clampedSelection = std::clamp(selection, 0, totalItemCount - 1);
	const float selectedHeight = CheatRowHeight(cheats, clampedSelection, scale);
	const float aboveBudget = std::max(0.0f, (maxMenuHeight - selectedHeight) * 0.5f);

	float aboveHeight = 0.0f;
	*firstItem = clampedSelection;
	while (*firstItem > 0) {
		const float previousHeight = CheatRowHeight(cheats, *firstItem - 1, scale);
		if (aboveHeight + previousHeight > aboveBudget) {
			break;
		}
		aboveHeight += previousHeight;
		(*firstItem)--;
	}

	float visibleHeight = 0.0f;
	int visible = 0;
	for (int i = *firstItem; i < totalItemCount; ++i) {
		const float rowHeight = CheatRowHeight(cheats, i, scale);
		if (visible > 0 && visibleHeight + rowHeight > maxMenuHeight) {
			break;
		}
		visibleHeight += rowHeight;
		visible++;
	}

	while (*firstItem > 0 && *firstItem + visible >= totalItemCount) {
		const float previousHeight = CheatRowHeight(cheats, *firstItem - 1, scale);
		if (visibleHeight + previousHeight > maxMenuHeight) {
			break;
		}
		visibleHeight += previousHeight;
		(*firstItem)--;
		visible++;
	}

	*visibleItemCount = std::max(1, visible);
	*contentHeight = visibleHeight;
}

}  // namespace

bool Overlay::Init(Draw::DrawContext *draw, const char *gamePath, LogCallback log) {
	if (log) {
		log_ = std::move(log);
	}
	if (!draw) {
		return false;
	}
	if (ready_) {
		return true;
	}
	TranslationManager::Instance().Init(log_);

	IMGUI_CHECKVERSION();
	context_ = ImGui::CreateContext();
	if (!context_) {
		return false;
	}

	ImGui::SetCurrentContext(context_);
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	io.DisplaySize = ImVec2((float)std::max(1, g_display.pixel_xres), (float)std::max(1, g_display.pixel_yres));

	ImGui::StyleColorsDark();
	ImGuiStyle &style = ImGui::GetStyle();
	style.WindowRounding = 18.0f;
	style.FrameRounding = 12.0f;
	style.GrabRounding = 12.0f;

	const char *const titleFontPaths[] = {
		"romfs:/fonts/font.ttf",
		"sdmc:/tico/fonts/font.ttf",
		"sdmc:/tico/assets/fonts/font.ttf",
		"sdmc:/tico/assets/font.ttf",
	};
	const char *const descriptionFontPaths[] = {
		"romfs:/fonts/description.ttf",
		"sdmc:/tico/fonts/description.ttf",
		"sdmc:/tico/assets/fonts/description.ttf",
		"sdmc:/tico/assets/description.ttf",
	};
	size_t titleFontSize = 0;
	size_t descriptionFontSize = 0;
	const char *loadedTitleFont = nullptr;
	const char *loadedDescriptionFont = nullptr;
	uint8_t *titleFont = LoadFirstImGuiFontData(titleFontPaths, sizeof(titleFontPaths) / sizeof(titleFontPaths[0]), &titleFontSize, &loadedTitleFont);
	uint8_t *descriptionFont = LoadFirstImGuiFontData(descriptionFontPaths, sizeof(descriptionFontPaths) / sizeof(descriptionFontPaths[0]), &descriptionFontSize, &loadedDescriptionFont);
	LogMessage(log_, "tico overlay font title=%s size=%u description=%s size=%u",
		loadedTitleFont ? loadedTitleFont : "<default>", (unsigned)titleFontSize,
		loadedDescriptionFont ? loadedDescriptionFont : "<default>", (unsigned)descriptionFontSize);

	if (!ImGui_ImplThin3d_Init(draw, titleFont, titleFontSize, descriptionFont, descriptionFontSize)) {
		ImGui::DestroyContext(context_);
		context_ = nullptr;
		return false;
	}

	const float loadedFontSize = io.Fonts->Fonts.Size > 0 ? io.Fonts->Fonts[0]->FontSize : 21.0f;
	if (loadedFontSize > 0.0f) {
		io.FontGlobalScale = Display::FontSize / loadedFontSize;
	}
	LogMessage(log_, "tico overlay font scale=%.3f base=%.1f target=%.1f",
		io.FontGlobalScale, loadedFontSize, Display::FontSize);

	title_ = GameTitleFromPath(gamePath);
	displaySettings_ = LoadPpssppDisplaySettings(log_);
	LoadSocial(draw);
	ready_ = true;
	const Result psmRc = psmInitialize();
	psmReady_ = R_SUCCEEDED(psmRc);
	batteryTimer_ = 0.0f;
	LogMessage(log_, "tico overlay psm=0x%x ready=%d", (unsigned)psmRc, psmReady_ ? 1 : 0);
	LogMessage(log_, "tico overlay initialized title=%s", title_.c_str());
	return true;
}

bool Overlay::LoadAvatarTextureFromMemory(Draw::DrawContext *draw, const unsigned char *data, size_t size, const char *tag) {
	if (!draw || !data || size == 0) {
		return false;
	}

	Draw::Texture *texture = CreateTextureFromFileData(draw, (const uint8_t *)data, size, ImageFileType::DETECT, false, tag);
	if (!texture) {
		return false;
	}

	ReleaseAvatarTexture();
	avatarTexture_ = texture;
	avatarWidth_ = texture->Width();
	avatarHeight_ = texture->Height();
	if (avatarWidth_ <= 0 || avatarHeight_ <= 0) {
		ReleaseAvatarTexture();
		return false;
	}
	return true;
}

bool Overlay::LoadAvatarTextureFromFile(Draw::DrawContext *draw, const char *path) {
	std::vector<unsigned char> data;
	if (!ReadFileBytes(path, &data)) {
		return false;
	}
	return LoadAvatarTextureFromMemory(draw, data.data(), data.size(), path);
}

void Overlay::LoadSocial(Draw::DrawContext *draw) {
	nickname_ = "Player 1";

	for (const char *path : kCustomAvatarPaths) {
		if (LoadAvatarTextureFromFile(draw, path)) {
			LogMessage(log_, "tico overlay avatar custom path=%s size=%dx%d", path, avatarWidth_, avatarHeight_);
			return;
		}
	}

#ifdef __SWITCH__
	const Result initRc = accountInitialize(AccountServiceType_Application);
	if (R_FAILED(initRc)) {
		LogMessage(log_, "tico overlay account init failed rc=0x%x", (unsigned)initRc);
		return;
	}

	AccountUid uid = {};
	if (!FindAccountUid(&uid)) {
		LogMessage(log_, "tico overlay account user not found");
		accountExit();
		return;
	}

	AccountProfile profile;
	const Result profileRc = accountGetProfile(&profile, uid);
	if (R_FAILED(profileRc)) {
		LogMessage(log_, "tico overlay account profile failed rc=0x%x", (unsigned)profileRc);
		accountExit();
		return;
	}

	AccountProfileBase profileBase = {};
	if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &profileBase))) {
		const std::string profileName = Trim(std::string(profileBase.nickname));
		if (!profileName.empty()) {
			nickname_ = profileName;
		}
	}

	u32 imageSize = 0;
	if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &imageSize)) && imageSize > 0) {
		std::vector<unsigned char> jpegData(imageSize);
		u32 actualSize = 0;
		if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpegData.data(), imageSize, &actualSize)) && actualSize > 0) {
			if (LoadAvatarTextureFromMemory(draw, jpegData.data(), actualSize, "switch-account-avatar")) {
				LogMessage(log_, "tico overlay avatar account user=%s size=%dx%d", nickname_.c_str(), avatarWidth_, avatarHeight_);
			}
		}
	}

	accountProfileClose(&profile);
	accountExit();
#endif
}

void Overlay::ReleaseAvatarTexture() {
	if (avatarTexture_) {
		avatarTexture_->Release();
		avatarTexture_ = nullptr;
	}
	avatarWidth_ = 0;
	avatarHeight_ = 0;
}

Draw::Texture *Overlay::LoadRAIconTexture(Draw::DrawContext *draw) {
	if (raIconTexture_ || !draw) {
		return raIconTexture_;
	}

	const char *const raIconPaths[] = {
		"romfs:/assets/ra.svg",
		"sdmc:/tico/assets/ra.svg",
	};
	for (const char *path : raIconPaths) {
		std::vector<unsigned char> svgData;
		if (!ReadFileBytes(path, &svgData)) {
			continue;
		}
		svgData.push_back('\0');
		NSVGimage *image = nsvgParse((char *)svgData.data(), "px", 96.0f);
		if (!image || image->width <= 0.0f || image->height <= 0.0f) {
			if (image) {
				nsvgDelete(image);
			}
			continue;
		}

		const float targetSize = 96.0f;
		const float svgMax = std::max(image->width, image->height);
		const float rasterScale = targetSize / svgMax;
		const int width = std::max(1, (int)std::ceil(image->width * rasterScale));
		const int height = std::max(1, (int)std::ceil(image->height * rasterScale));
		std::vector<unsigned char> pixels((size_t)width * (size_t)height * 4);
		NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
		if (rasterizer) {
			nsvgRasterize(rasterizer, image, 0.0f, 0.0f, rasterScale, pixels.data(), width, height, width * 4);
			Draw::TextureDesc desc{};
			desc.type = Draw::TextureType::LINEAR2D;
			desc.format = Draw::DataFormat::R8G8B8A8_UNORM;
			desc.width = width;
			desc.height = height;
			desc.depth = 1;
			desc.mipLevels = 1;
			desc.generateMips = false;
			desc.tag = path;
			desc.initData.push_back(pixels.data());
			raIconTexture_ = draw->CreateTexture(desc);
			nsvgDeleteRasterizer(rasterizer);
		}
		nsvgDelete(image);
		if (raIconTexture_) {
			LogMessage(log_, "tico overlay RA icon loaded path=%s", path);
			return raIconTexture_;
		}
	}

	LogMessage(log_, "tico overlay RA icon not found");
	return nullptr;
}

Draw::Texture *Overlay::LoadLoaderTexture(Draw::DrawContext *draw) {
	if (loaderTexture_ || !draw) {
		return loaderTexture_;
	}

	const char *const loaderPaths[] = {
		"romfs:/assets/loader.svg",
		"sdmc:/tico/assets/loader.svg",
	};
	for (const char *path : loaderPaths) {
		std::vector<unsigned char> svgData;
		if (!ReadFileBytes(path, &svgData)) {
			continue;
		}
		svgData.push_back('\0');
		NSVGimage *image = nsvgParse((char *)svgData.data(), "px", 96.0f);
		if (!image || image->width <= 0.0f || image->height <= 0.0f) {
			if (image) {
				nsvgDelete(image);
			}
			continue;
		}

		const float targetSize = 96.0f;
		const float svgMax = std::max(image->width, image->height);
		const float rasterScale = targetSize / svgMax;
		const int width = std::max(1, (int)std::ceil(image->width * rasterScale));
		const int height = std::max(1, (int)std::ceil(image->height * rasterScale));
		std::vector<unsigned char> pixels((size_t)width * (size_t)height * 4);
		NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
		if (rasterizer) {
			nsvgRasterize(rasterizer, image, 0.0f, 0.0f, rasterScale, pixels.data(), width, height, width * 4);
			Draw::TextureDesc desc{};
			desc.type = Draw::TextureType::LINEAR2D;
			desc.format = Draw::DataFormat::R8G8B8A8_UNORM;
			desc.width = width;
			desc.height = height;
			desc.depth = 1;
			desc.mipLevels = 1;
			desc.generateMips = false;
			desc.tag = path;
			desc.initData.push_back(pixels.data());
			loaderTexture_ = draw->CreateTexture(desc);
			nsvgDeleteRasterizer(rasterizer);
		}
		nsvgDelete(image);
		if (loaderTexture_) {
			LogMessage(log_, "tico overlay loader loaded path=%s", path);
			return loaderTexture_;
		}
	}

	LogMessage(log_, "tico overlay loader not found");
	return nullptr;
}

void Overlay::ReleaseRAIconTexture() {
	if (raIconTexture_) {
		raIconTexture_->Release();
		raIconTexture_ = nullptr;
	}
}

void Overlay::ReleaseLoaderTexture() {
	if (loaderTexture_) {
		loaderTexture_->Release();
		loaderTexture_ = nullptr;
	}
}

void Overlay::Shutdown() {
	if (!ready_ && !context_) {
		return;
	}

	if (context_) {
		ImGui::SetCurrentContext(context_);
	}
	ReleaseAvatarTexture();
	ReleaseRAIconTexture();
	ReleaseLoaderTexture();
	if (ready_) {
		ImGui_ImplThin3d_Shutdown();
	}
	if (context_) {
		ImGui::DestroyContext(context_);
	}
	if (psmReady_) {
		psmExit();
	}

	context_ = nullptr;
	ready_ = false;
	visible_ = false;
	comboDown_ = false;
	exitRequested_ = false;
	menu_ = Menu::Quick;
	selection_ = 0;
	settingsSelection_ = 0;
	displaySettings_ = {};
	pendingCommand_ = {};
	slotInUse_.fill(false);
	cheatsEnabled_ = false;
	cheatsAvailable_ = false;
	cheatsLoading_ = false;
	cheatsLoadCommandSent_ = false;
	cheatsLoadingDelayFrames_ = 0;
	cheats_.clear();
	lastAnalogNavMs_ = 0;
	nextCheatVerticalNavMs_ = 0;
	nextCheatHorizontalNavMs_ = 0;
	cheatVerticalNavDir_ = 0;
	cheatHorizontalNavDir_ = 0;
	nickname_ = "Player 1";
	loaderTimer_ = 0.0f;
	psmReady_ = false;
}

void Overlay::SetVisible(bool visible) {
	if (visible_ == visible) {
		return;
	}

	visible_ = visible;
	menu_ = Menu::Quick;
	selection_ = 0;
	settingsSelection_ = 0;
	animTimer_ = 0.0f;
	lastAnalogNavMs_ = 0;
	nextCheatVerticalNavMs_ = 0;
	nextCheatHorizontalNavMs_ = 0;
	cheatVerticalNavDir_ = 0;
	cheatHorizontalNavDir_ = 0;
	if (visible_) {
		TranslationManager::Instance().Init(log_);
		displaySettings_ = LoadPpssppDisplaySettings(log_);
	} else {
		cheatsLoading_ = false;
		cheatsLoadCommandSent_ = false;
		cheatsLoadingDelayFrames_ = 0;
	}
	LogMessage(log_, "tico overlay visible=%d", visible ? 1 : 0);
}

void Overlay::SetSaveStateInfo(int currentSlot, const std::array<bool, Ppsspp::SaveStateSlotCount> &slotInUse) {
	currentStateSlot_ = std::clamp(currentSlot, 0, Ppsspp::SaveStateSlotCount - 1);
	slotInUse_ = slotInUse;
}

void Overlay::SetCheatsEnabled(bool enabled) {
	cheatsEnabled_ = enabled;
	if (!enabled) {
		cheatsAvailable_ = false;
		cheatsLoading_ = false;
		cheatsLoadCommandSent_ = false;
		cheatsLoadingDelayFrames_ = 0;
		cheats_.clear();
		if (menu_ == Menu::Cheats) {
			menu_ = Menu::Quick;
			selection_ = 0;
			animTimer_ = kOverlayAnimDuration;
		}
	}
}

void Overlay::SetCheatInfo(bool enabled, bool available, const std::vector<CheatMenuEntry> &entries) {
	const bool wasLoading = cheatsLoading_;
	cheatsEnabled_ = enabled;
	cheatsAvailable_ = available;
	cheatsLoading_ = false;
	cheatsLoadCommandSent_ = false;
	cheatsLoadingDelayFrames_ = 0;
	cheats_ = entries;
	if (wasLoading && visible_ && menu_ == Menu::Quick) {
		menu_ = Menu::Cheats;
		selection_ = FirstSelectableCheatRow(cheats_);
		animTimer_ = kOverlayAnimDuration;
	} else if (menu_ == Menu::Cheats && !IsSelectableCheatRow(cheats_, selection_)) {
		selection_ = FirstSelectableCheatRow(cheats_);
	}
}

void Overlay::ReloadDisplaySettings() {
	displaySettings_ = LoadPpssppDisplaySettings(log_);
}

OverlayCommand Overlay::ConsumeCommand() {
	if (pendingCommand_.action == OverlayAction::None && cheatsLoading_ && !cheatsLoadCommandSent_) {
		if (cheatsLoadingDelayFrames_ > 0) {
			cheatsLoadingDelayFrames_--;
			return {};
		}
		cheatsLoadCommandSent_ = true;
		return { OverlayAction::LoadCheats, 0 };
	}
	OverlayCommand command = pendingCommand_;
	pendingCommand_ = {};
	return command;
}

int Overlay::QuickMenuStorageIndex(int visibleIndex) const {
	int visible = 0;
	for (int i = 0; i < (int)(sizeof(kQuickMenuItems) / sizeof(kQuickMenuItems[0])); ++i) {
		const QuickMenuItem &item = kQuickMenuItems[i];
		if (item.action == QuickMenuItem::Action::Cheats && !cheatsEnabled_) {
			continue;
		}
		if (visible == visibleIndex) {
			return i;
		}
		visible++;
	}
	return 0;
}

int Overlay::ItemCount() const {
	if (menu_ == Menu::Quick) {
		int count = 0;
		for (const QuickMenuItem &item : kQuickMenuItems) {
			if (item.action != QuickMenuItem::Action::Cheats || cheatsEnabled_) {
				count++;
			}
		}
		return count;
	}
	if (menu_ == Menu::SaveStates) {
		return Ppsspp::SaveStateSlotCount;
	}
	if (menu_ == Menu::Cheats) {
		return std::max(1, (int)cheats_.size());
	}
	return 2;
}

void Overlay::ApplyDisplaySettings(bool save) {
	displaySettings_ = NormalizePpssppDisplaySettingsForCurrentMode(displaySettings_);
	ApplyPpssppDisplaySettings(displaySettings_);
	if (save) {
		SavePpssppDisplaySettings(displaySettings_, log_);
	}
}

void Overlay::CycleSetting(int direction) {
	if (direction == 0) {
		return;
	}

	displaySettings_ = NormalizePpssppDisplaySettingsForCurrentMode(displaySettings_);
	if (settingsSelection_ == 0) {
		displaySettings_.mode = displaySettings_.mode == DisplayMode::Integer ? DisplayMode::Display : DisplayMode::Integer;
		displaySettings_.size = displaySettings_.mode == DisplayMode::Integer ? DisplaySize::Auto : DisplaySize::_16_9;
	} else {
		if (displaySettings_.mode == DisplayMode::Integer) {
			DisplaySize integerSizes[5];
			const int integerSizeCount = GetAvailableIntegerDisplaySizes(integerSizes, (int)(sizeof(integerSizes) / sizeof(integerSizes[0])));
			displaySettings_.size = CycleDisplaySize(displaySettings_.size, integerSizes, integerSizeCount, direction);
		} else {
			displaySettings_.size = CycleDisplaySize(displaySettings_.size, kAspectDisplaySizes,
				(int)(sizeof(kAspectDisplaySizes) / sizeof(kAspectDisplaySizes[0])), direction);
		}
	}
	ApplyDisplaySettings(true);
}

void Overlay::ExecuteSelection() {
	const int itemCount = ItemCount();
	if (selection_ < 0 || selection_ >= itemCount) {
		return;
	}

	if (menu_ == Menu::Settings) {
		CycleSetting(1);
		return;
	}

	if (menu_ == Menu::SaveStates) {
		if (saveStateMode_ == OverlayAction::LoadState && !slotInUse_[selection_]) {
			LogMessage(log_, "tico load state ignored empty slot=%d", selection_);
			return;
		}
		pendingCommand_ = { saveStateMode_, selection_ };
		SetVisible(false);
		return;
	}

	if (menu_ == Menu::Cheats) {
		if (cheats_.empty() || !cheats_[selection_].toggleable || cheats_[selection_].sourceIndex < 0) {
			return;
		}
		pendingCommand_ = { OverlayAction::ToggleCheat, cheats_[selection_].sourceIndex };
		return;
	}

	const QuickMenuItem &item = kQuickMenuItems[QuickMenuStorageIndex(selection_)];
	if (item.action == QuickMenuItem::Action::SaveState || item.action == QuickMenuItem::Action::LoadState) {
		saveStateMode_ = item.action == QuickMenuItem::Action::SaveState ? OverlayAction::SaveState : OverlayAction::LoadState;
		menu_ = Menu::SaveStates;
		selection_ = currentStateSlot_;
		animTimer_ = kOverlayAnimDuration;
	} else if (item.action == QuickMenuItem::Action::Cheats) {
		if (cheatsLoading_) {
			return;
		}
		cheatsLoading_ = true;
		cheatsLoadCommandSent_ = false;
		cheatsLoadingDelayFrames_ = 1;
		loaderTimer_ = 0.0f;
		pendingCommand_ = {};
	} else if (item.action == QuickMenuItem::Action::Settings) {
		menu_ = Menu::Settings;
		selection_ = 0;
		settingsSelection_ = 0;
		animTimer_ = kOverlayAnimDuration;
	} else {
		exitRequested_ = true;
	}
}

bool Overlay::HandleInput(u64 buttons, u64 pressed, int leftStickX, int leftStickY, int rightStickX, int rightStickY) {
	const bool wasVisible = visible_;
	const bool comboDown = (buttons & HidNpadButton_Plus) && (buttons & HidNpadButton_Minus);
	if (comboDown && !comboDown_) {
		if (!visible_) {
			SetVisible(true);
		} else if (menu_ != Menu::Quick) {
			menu_ = Menu::Quick;
			selection_ = 0;
			settingsSelection_ = 0;
			animTimer_ = kOverlayAnimDuration;
		} else {
			SetVisible(false);
		}
	}
	comboDown_ = comboDown;

	if (visible_) {
		const int itemCount = ItemCount();
		bool navUp = (pressed & HidNpadButton_Up) != 0;
		bool navDown = (pressed & HidNpadButton_Down) != 0;
		bool navLeft = (pressed & HidNpadButton_Left) != 0;
		bool navRight = (pressed & HidNpadButton_Right) != 0;
		if (menu_ == Menu::Cheats) {
			navUp = false;
			navDown = false;
			navLeft = false;
			navRight = false;

			const bool analogUp = leftStickY > kCheatAnalogNavThreshold || rightStickY > kCheatAnalogNavThreshold;
			const bool analogDown = leftStickY < -kCheatAnalogNavThreshold || rightStickY < -kCheatAnalogNavThreshold;
			const bool analogLeft = leftStickX < -kCheatAnalogNavThreshold || rightStickX < -kCheatAnalogNavThreshold;
			const bool analogRight = leftStickX > kCheatAnalogNavThreshold || rightStickX > kCheatAnalogNavThreshold;
			const int verticalPressedDir = (pressed & HidNpadButton_Up) ? -1 : ((pressed & HidNpadButton_Down) ? 1 : 0);
			const int verticalHeldDir = ((buttons & HidNpadButton_Up) || analogUp) ? -1 : (((buttons & HidNpadButton_Down) || analogDown) ? 1 : 0);
			const int horizontalPressedDir = (pressed & HidNpadButton_Left) ? -1 : ((pressed & HidNpadButton_Right) ? 1 : 0);
			const int horizontalHeldDir = ((buttons & HidNpadButton_Left) || analogLeft) ? -1 : (((buttons & HidNpadButton_Right) || analogRight) ? 1 : 0);
			const u64 nowMs = CurrentTimeMs();

			if (verticalPressedDir != 0) {
				cheatVerticalNavDir_ = verticalPressedDir;
				nextCheatVerticalNavMs_ = nowMs + kCheatVerticalNavInitialRepeatMs;
				navUp = verticalPressedDir < 0;
				navDown = verticalPressedDir > 0;
			} else if (HeldNavigationTriggered(verticalHeldDir, cheatVerticalNavDir_, nextCheatVerticalNavMs_,
				kCheatVerticalNavInitialRepeatMs, kCheatVerticalNavRepeatMs, nowMs)) {
				navUp = verticalHeldDir < 0;
				navDown = verticalHeldDir > 0;
			}

			if (!navUp && !navDown) {
				if (horizontalPressedDir != 0) {
					cheatHorizontalNavDir_ = horizontalPressedDir;
					nextCheatHorizontalNavMs_ = nowMs + kCheatHorizontalNavInitialRepeatMs;
					navLeft = horizontalPressedDir < 0;
					navRight = horizontalPressedDir > 0;
				} else if (HeldNavigationTriggered(horizontalHeldDir, cheatHorizontalNavDir_, nextCheatHorizontalNavMs_,
					kCheatHorizontalNavInitialRepeatMs, kCheatHorizontalNavRepeatMs, nowMs)) {
					navLeft = horizontalHeldDir < 0;
					navRight = horizontalHeldDir > 0;
				}
			}

			lastAnalogNavMs_ = 0;
		} else {
			cheatVerticalNavDir_ = 0;
			cheatHorizontalNavDir_ = 0;
			nextCheatVerticalNavMs_ = 0;
			nextCheatHorizontalNavMs_ = 0;

			const bool analogUp = leftStickY > kAnalogNavThreshold || rightStickY > kAnalogNavThreshold;
			const bool analogDown = leftStickY < -kAnalogNavThreshold || rightStickY < -kAnalogNavThreshold;
			const bool analogLeft = leftStickX < -kAnalogNavThreshold || rightStickX < -kAnalogNavThreshold;
			const bool analogRight = leftStickX > kAnalogNavThreshold || rightStickX > kAnalogNavThreshold;
			if (analogUp || analogDown || analogLeft || analogRight) {
				const u64 nowMs = CurrentTimeMs();
				if (nowMs == 0 || nowMs - lastAnalogNavMs_ >= kAnalogNavRepeatMs) {
					navUp = navUp || analogUp;
					navDown = navDown || (!analogUp && analogDown);
					if (!analogUp && !analogDown) {
						navLeft = navLeft || analogLeft;
						navRight = navRight || (!analogLeft && analogRight);
					}
					lastAnalogNavMs_ = nowMs;
				}
			} else {
				lastAnalogNavMs_ = 0;
			}
		}

		if (menu_ == Menu::Cheats) {
			if (navUp) {
				MoveCheatSelectionWrapped(selection_, cheats_, -1);
			}
			if (navDown) {
				MoveCheatSelectionWrapped(selection_, cheats_, 1);
			}
			if (navLeft) {
				MoveCheatSelectionWrapped(selection_, cheats_, -kCheatPageStep);
			}
			if (navRight) {
				MoveCheatSelectionWrapped(selection_, cheats_, kCheatPageStep);
			}
		} else {
			if (navUp && itemCount > 0) {
				selection_ = (selection_ + itemCount - 1) % itemCount;
			}
			if (navDown && itemCount > 0) {
				selection_ = (selection_ + 1) % itemCount;
			}
		}
		if (menu_ == Menu::Settings) {
			settingsSelection_ = selection_;
			if (navLeft) {
				CycleSetting(-1);
			}
			if (navRight) {
				CycleSetting(1);
			}
		}
		if (pressed & HidNpadButton_A) {
			ExecuteSelection();
		}
		if (menu_ == Menu::Quick && (pressed & HidNpadButton_Minus) && !(buttons & HidNpadButton_Plus)) {
			pendingCommand_ = { OverlayAction::Reset, 0 };
			SetVisible(false);
			return true;
		}
		if (pressed & HidNpadButton_B) {
			if (menu_ != Menu::Quick) {
				menu_ = Menu::Quick;
				selection_ = 0;
				settingsSelection_ = 0;
				animTimer_ = kOverlayAnimDuration;
			} else {
				SetVisible(false);
			}
		}
	}

	return wasVisible || visible_ || comboDown;
}

void Overlay::UpdateBattery(float deltaTime) {
	if (!psmReady_) {
		return;
	}

	batteryTimer_ -= deltaTime;
	if (batteryTimer_ > 0.0f) {
		return;
	}
	batteryTimer_ = 1.0f;

	u32 batteryLevel = batteryLevel_;
	if (R_SUCCEEDED(psmGetBatteryChargePercentage(&batteryLevel))) {
		batteryLevel_ = batteryLevel;
	}

	PsmChargerType chargerType = PsmChargerType_Unconnected;
	if (R_SUCCEEDED(psmGetChargerType(&chargerType))) {
		charging_ = chargerType != PsmChargerType_Unconnected;
	}
}

void Overlay::DrawBackground(ImDrawList *drawList, ImVec2 displaySize, float ease) {
	const int baseAlpha = (int)(200.0f * ease);
	const int maxAlpha = (int)(250.0f * ease);
	if (baseAlpha <= 0) {
		return;
	}

	const float topH = displaySize.y * 0.20f;
	const float bottomH = displaySize.y * 0.20f;
	const float centerH = displaySize.y - topH - bottomH;
	const ImU32 colMax = IM_COL32(0, 0, 0, maxAlpha);
	const ImU32 colBase = IM_COL32(0, 0, 0, baseAlpha);

	drawList->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(displaySize.x, topH), colMax, colMax, colBase, colBase);
	drawList->AddRectFilled(ImVec2(0.0f, topH), ImVec2(displaySize.x, topH + centerH), colBase);
	drawList->AddRectFilledMultiColor(ImVec2(0.0f, displaySize.y - bottomH), displaySize, colBase, colBase, colMax, colMax);
}

std::string Overlay::TitleText() const {
	if (menu_ == Menu::SaveStates) {
		return saveStateMode_ == OverlayAction::SaveState ? tr("emulator_save_state") : tr("emulator_load_state");
	}
	if (menu_ == Menu::Cheats) {
		return tr("emulator_cheats");
	}
	if (menu_ == Menu::Settings) {
		return tr("emulator_settings");
	}

	return title_.empty() ? "PPSSPP" : title_;
}

void Overlay::DrawTitle(ImDrawList *drawList, ImVec2 displaySize, float scale, float ease) {
	std::string title = TitleText();
	if (title.size() > 50) {
		title = title.substr(0, 47) + "...";
	}

	ImFont *font = ImGui::GetFont();
	const float titleHeight = 72.0f * scale;
	const float availableTopSpace = 110.0f * scale;
	const float cardWidth = displaySize.x * 0.4f;
	const float cardX = (displaySize.x - cardWidth) * 0.5f;
	const float cardY = (availableTopSpace - titleHeight) * 0.5f;
	const float startY = -150.0f * scale;
	const float currentY = startY + (cardY - startY) * ease;
	const float fontSize = ImGui::GetFontSize();
	const ImVec2 textSize = font->CalcTextSizeA(fontSize, 10000.0f, 0.0f, title.c_str());
	const float textX = cardX + (cardWidth - textSize.x) * 0.5f;
	const float textY = currentY + (titleHeight - textSize.y) * 0.5f;

	DrawOverlayText(drawList, font, fontSize, ImVec2(textX, textY), IM_COL32(200, 200, 200, (int)(255.0f * ease)), title);
}

void Overlay::DrawSocialArea(ImDrawList *drawList, ImVec2 displaySize, float scale, float ease) {
	if (ease < 0.01f) {
		return;
	}

	const float startOffset = 200.0f * scale;
	const float currentOffset = startOffset * (1.0f - ease);
	const float avatarSize = 72.0f * scale;
	const float sideMargin = 32.0f * scale;
	const float topMargin = 32.0f * scale;
	const float barHeight = 50.0f * scale;
	const float radius = avatarSize * 0.5f;
	const ImVec2 avatarCenter(
		sideMargin + avatarSize * 0.5f - currentOffset,
		topMargin + barHeight * 0.5f);

	drawList->AddCircleFilled(avatarCenter, radius, IM_COL32(45, 45, 45, (int)(255.0f * ease)), 32);

	const float imageRadius = radius - (4.0f * scale);
	const ImVec2 imageMin(avatarCenter.x - imageRadius, avatarCenter.y - imageRadius);
	const ImVec2 imageMax(avatarCenter.x + imageRadius, avatarCenter.y + imageRadius);
	if (avatarTexture_) {
		const ImTextureID textureId = ImGui_ImplThin3d_AddTextureTemp(avatarTexture_);
		drawList->AddImageRounded(textureId, imageMin, imageMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
			IM_COL32(255, 255, 255, (int)(255.0f * ease)), imageRadius);
		drawList->AddCircle(avatarCenter, imageRadius, IM_COL32(255, 255, 255, (int)(60.0f * ease)), 32, 1.0f * scale);
	} else {
		drawList->AddCircleFilled(avatarCenter, imageRadius, IM_COL32(200, 200, 210, (int)(255.0f * ease)), 32);
	}
}

void Overlay::DrawMenu(ImDrawList *drawList, ImVec2 displaySize, float scale, float ease) {
	const int totalItemCount = ItemCount();
	const float itemHeight = 64.0f * scale;
	int visibleItemCount = totalItemCount;
	int firstItem = 0;
	float contentHeight = (float)visibleItemCount * itemHeight;
	if (menu_ == Menu::Cheats) {
		CalculateVisibleCheatRows(cheats_, totalItemCount, selection_, displaySize.y, scale, &firstItem, &visibleItemCount, &contentHeight);
	}
	const float targetMenuWidth = kQuickMenuWidth * (menu_ == Menu::Cheats ? 2.0f : 1.0f) * scale;
	const float menuWidth = std::min(targetMenuWidth, displaySize.x - 96.0f * scale);
	const ImVec2 menuSize(menuWidth, contentHeight);
	const float targetY = (displaySize.y - menuSize.y) * 0.5f;
	const float startY = displaySize.y + (100.0f * scale);
	const float currentY = startY + (targetY - startY) * ease;
	const ImVec2 menuPos((displaySize.x - menuSize.x) * 0.5f, currentY);
	const ImVec2 menuMax(menuPos.x + menuSize.x, menuPos.y + menuSize.y);
	const float cornerRadius = 16.0f * scale;

	drawList->AddRectFilled(menuPos, menuMax, IM_COL32(45, 45, 45, (int)(255.0f * ease)), cornerRadius);

	ImFont *font = ImGui::GetFont();
	const float labelSize = ImGui::GetFontSize() * (menu_ == Menu::Cheats ? kCheatTextScale : 0.85f);
	float itemY = menuPos.y;
	for (int row = 0; row < visibleItemCount; ++row) {
		const int i = firstItem + row;
		const bool selected = selection_ == i && (menu_ != Menu::Cheats || IsSelectableCheatRow(cheats_, i));
		const float rowHeight = menu_ == Menu::Cheats ? CheatRowHeight(cheats_, i, scale) : itemHeight;
		const float rowTopPadding = menu_ == Menu::Cheats ? CheatRowTopPadding(cheats_, i, scale) : 0.0f;
		const ImVec2 itemMin(menuPos.x, itemY);
		const ImVec2 itemMax(menuPos.x + menuSize.x, itemY + rowHeight);

		if (selected) {
			ImDrawFlags corners = ImDrawFlags_None;
			float itemRadius = 0.0f;
			if (row == 0) {
				corners = ImDrawFlags_RoundCornersTop;
				itemRadius = cornerRadius;
			} else if (row == visibleItemCount - 1) {
				corners = ImDrawFlags_RoundCornersBottom;
				itemRadius = cornerRadius;
			}
			drawList->AddRectFilled(itemMin, itemMax, IM_COL32(60, 60, 60, (int)(255.0f * ease)), itemRadius, corners);
		}

		char slotLabel[128];
		std::string labelText;
		std::string valueText;
		bool drawCheckbox = false;
		bool checkboxChecked = false;
		bool drawLoader = false;
		const bool tipRow = menu_ == Menu::Cheats && !cheats_.empty() && cheats_[i].kind == CheatMenuEntryKind::Tip;
		const bool sectionRow = menu_ == Menu::Cheats && !cheats_.empty() && cheats_[i].kind == CheatMenuEntryKind::Section;
		const bool metadataRow = menu_ == Menu::Cheats && !cheats_.empty() && !cheats_[i].toggleable;
		ImFont *rowFont = tipRow ? ImGui_GetFixedFont() : font;
		if (!rowFont) {
			rowFont = font;
		}
		const float rowLabelSize = tipRow ? ImGui::GetFontSize() * kCheatTipTextScale :
			(sectionRow ? ImGui::GetFontSize() * kCheatSectionTextScale : labelSize);
		const char *label = nullptr;
		const char *value = nullptr;
		if (menu_ == Menu::Quick) {
			const QuickMenuItem &item = kQuickMenuItems[QuickMenuStorageIndex(i)];
			labelText = tr(item.labelKey);
			drawLoader = cheatsLoading_ && item.action == QuickMenuItem::Action::Cheats;
			label = labelText.c_str();
		} else if (menu_ == Menu::SaveStates) {
			const std::string slotFormat = tr("emulator_slot");
			const std::string slotState = slotInUse_[i] ? tr("emulator_in_use") : tr("emulator_empty");
			snprintf(slotLabel, sizeof(slotLabel), slotFormat.c_str(), i + 1, slotState.c_str());
			label = slotLabel;
		} else if (menu_ == Menu::Cheats) {
			if (cheats_.empty()) {
				labelText = cheatsAvailable_ ? tr("emulator_no_cheats") : tr("emulator_no_cheat_db");
			} else {
				labelText = cheats_[i].name.empty() ? tr("emulator_cheat") : cheats_[i].name;
				drawCheckbox = cheats_[i].toggleable;
				checkboxChecked = cheats_[i].enabled;
			}
			label = labelText.c_str();
		} else {
			if (i == 0) {
				labelText = tr("emulator_display_mode");
				valueText = TranslatedDisplayModeLabel(displaySettings_.mode);
			} else {
				labelText = tr("emulator_size");
				valueText = TranslatedDisplaySizeLabel(displaySettings_.size);
			}
			label = labelText.c_str();
			value = valueText.c_str();
		}
		float textX = itemMin.x + (20.0f * scale);
		if (drawCheckbox) {
			textX += 34.0f * scale;
		}
		std::string drawLabelText;
		if (value) {
			const ImVec2 valueSize = rowFont->CalcTextSizeA(rowLabelSize, 10000.0f, 0.0f, value);
			const float valueX = itemMax.x - valueSize.x - (40.0f * scale);
			drawLabelText = TruncateToWidth(rowFont, rowLabelSize, label, std::max(20.0f * scale, valueX - textX - (16.0f * scale)));
			label = drawLabelText.c_str();
		} else if (menu_ == Menu::Cheats) {
			drawLabelText = TruncateToWidth(rowFont, rowLabelSize, label, std::max(20.0f * scale, itemMax.x - textX - (36.0f * scale)));
			label = drawLabelText.c_str();
		}
		const ImVec2 textSize = rowFont->CalcTextSizeA(rowLabelSize, 10000.0f, 0.0f, label);
		const float textAreaHeight = std::max(1.0f, rowHeight - rowTopPadding);
		const float textY = itemMin.y + rowTopPadding + (textAreaHeight - textSize.y) * 0.5f;
		const ImU32 textColor = metadataRow
			? IM_COL32(165, 165, 165, (int)(235.0f * ease))
			: (selected ? IM_COL32(255, 255, 255, (int)(255.0f * ease)) : IM_COL32(200, 200, 200, (int)(255.0f * ease)));
		if (drawCheckbox) {
			const float boxSize = 18.0f * scale;
			const float boxX = itemMin.x + 20.0f * scale;
			const float boxY = itemMin.y + (rowHeight - boxSize) * 0.5f;
			const ImVec2 boxMin(boxX, boxY);
			const ImVec2 boxMax(boxX + boxSize, boxY + boxSize);
			const ImU32 borderColor = selected ? IM_COL32(255, 255, 255, (int)(230.0f * ease)) : IM_COL32(180, 180, 180, (int)(210.0f * ease));
			const ImU32 fillColor = checkboxChecked ? IM_COL32(230, 230, 230, (int)(245.0f * ease)) : IM_COL32(30, 30, 30, (int)(130.0f * ease));
			drawList->AddRectFilled(boxMin, boxMax, fillColor, 4.0f * scale);
			drawList->AddRect(boxMin, boxMax, borderColor, 4.0f * scale, 0, 1.5f * scale);
			if (checkboxChecked) {
				const ImU32 checkColor = IM_COL32(40, 40, 40, (int)(255.0f * ease));
				drawList->AddLine(
					ImVec2(boxX + 4.0f * scale, boxY + 9.5f * scale),
					ImVec2(boxX + 7.5f * scale, boxY + 13.0f * scale),
					checkColor, 2.0f * scale);
				drawList->AddLine(
					ImVec2(boxX + 7.5f * scale, boxY + 13.0f * scale),
					ImVec2(boxX + 14.0f * scale, boxY + 5.0f * scale),
					checkColor, 2.0f * scale);
			}
		}
		drawList->AddText(rowFont, rowLabelSize, ImVec2(textX, textY), textColor, label);
		if (drawLoader) {
			const float loaderSize = 24.0f * scale;
			const ImVec2 loaderCenter(itemMax.x - 34.0f * scale, itemMin.y + rowHeight * 0.5f);
			const float angle = loaderTimer_ * 6.2831853f * 1.6f;
			if (loaderTexture_) {
				const ImTextureID textureId = ImGui_ImplThin3d_AddTextureTemp(loaderTexture_);
				DrawRotatingImage(drawList, textureId, loaderCenter, loaderSize, angle,
					IM_COL32(255, 255, 255, (int)(230.0f * ease)));
			} else {
				DrawFallbackSpinner(drawList, loaderCenter, loaderSize * 0.48f, angle, scale, ease);
			}
		}
		if (value) {
			const ImVec2 valueSize = rowFont->CalcTextSizeA(rowLabelSize, 10000.0f, 0.0f, value);
			const float valueX = itemMax.x - valueSize.x - (40.0f * scale);
			drawList->AddText(rowFont, rowLabelSize, ImVec2(valueX, textY), textColor, value);

			if (selected) {
				const float arrowSize = 12.0f * scale;
				const float arrowY = itemMin.y + (rowHeight - arrowSize) * 0.5f;
				const float leftArrowX = valueX - arrowSize - (12.0f * scale);
				drawList->AddTriangleFilled(
					ImVec2(leftArrowX, arrowY + arrowSize * 0.5f),
					ImVec2(leftArrowX + arrowSize, arrowY),
					ImVec2(leftArrowX + arrowSize, arrowY + arrowSize),
					textColor);

				const float rightArrowX = valueX + valueSize.x + (12.0f * scale);
				drawList->AddTriangleFilled(
					ImVec2(rightArrowX + arrowSize, arrowY + arrowSize * 0.5f),
					ImVec2(rightArrowX, arrowY),
					ImVec2(rightArrowX, arrowY + arrowSize),
					textColor);
			}
		}
		itemY += rowHeight;
	}

	if (menu_ == Menu::Cheats && totalItemCount > visibleItemCount) {
		const float trackWidth = 4.0f * scale;
		const float trackMargin = 8.0f * scale;
		const float trackX = menuMax.x - trackMargin - trackWidth;
		const float trackY = menuPos.y + trackMargin;
		const float trackHeight = menuSize.y - trackMargin * 2.0f;
		drawList->AddRectFilled(ImVec2(trackX, trackY), ImVec2(trackX + trackWidth, trackY + trackHeight),
			IM_COL32(80, 80, 80, (int)(180.0f * ease)), trackWidth * 0.5f);
		const float thumbHeight = std::max(24.0f * scale, trackHeight * ((float)visibleItemCount / (float)totalItemCount));
		const float scrollRange = std::max(1.0f, (float)(totalItemCount - visibleItemCount));
		const float thumbY = trackY + (trackHeight - thumbHeight) * ((float)firstItem / scrollRange);
		drawList->AddRectFilled(ImVec2(trackX, thumbY), ImVec2(trackX + trackWidth, thumbY + thumbHeight),
			IM_COL32(190, 190, 190, (int)(220.0f * ease)), trackWidth * 0.5f);
	}
}

void Overlay::DrawHelpers(ImDrawList *drawList, ImVec2 displaySize, float scale, float ease) {
	struct Helper {
		const char *button;
		std::string label;
	};
	std::vector<Helper> helpers;
	if (menu_ == Menu::Quick) {
		helpers.push_back({"-", tr("emulator_reset")});
	}
	if (menu_ == Menu::Settings) {
		helpers.push_back({"DPAD", tr("emulator_change")});
	}
	if (menu_ == Menu::Cheats && cheats_.size() > kCheatPageStep) {
		helpers.push_back({"<>", "+10"});
	}
	const bool canToggleCheat = menu_ == Menu::Cheats && !cheats_.empty() && selection_ >= 0 &&
		selection_ < (int)cheats_.size() && cheats_[selection_].toggleable;
	const std::string aLabel = menu_ == Menu::SaveStates
		? (saveStateMode_ == OverlayAction::SaveState ? tr("emulator_save_state") : tr("emulator_load_state"))
		: (canToggleCheat ? tr("emulator_toggle") : tr("emulator_select"));
	helpers.push_back({"B", tr("emulator_back")});
	helpers.push_back({"A", aLabel});

	ImFont *font = ImGui::GetFont();
	const float barHeight = 48.0f * scale;
	const float marginBottom = 24.0f * scale;
	const float padding = 16.0f * scale;
	const float buttonSize = 22.0f * scale;
	const float itemSpacing = 12.0f * scale;
	const float fontSize = ImGui::GetFontSize() * 0.78f;

	float totalWidth = padding * 2.0f;
	for (size_t i = 0; i < helpers.size(); ++i) {
		totalWidth += buttonSize + (8.0f * scale) + font->CalcTextSizeA(fontSize, 10000.0f, 0.0f, helpers[i].label.c_str()).x;
		if (i + 1 < helpers.size()) {
			totalWidth += itemSpacing;
		}
	}

	const float currentOffset = (kQuickMenuWidth * scale) * (1.0f - ease);
	const float barX = displaySize.x - totalWidth - 20.0f * scale + currentOffset;
	const float barY = displaySize.y - marginBottom - barHeight;
	const float centerY = barY + barHeight * 0.5f;
	float cursorX = barX + padding;
	const ImU32 textColor = IM_COL32(200, 200, 200, (int)(255.0f * ease));

	for (size_t i = 0; i < helpers.size(); ++i) {
		const Helper &helper = helpers[i];
		DrawSwitchButtonPrompt(drawList, font, fontSize, ImVec2(cursorX + buttonSize * 0.5f, centerY), buttonSize, helper.button, ease);
		cursorX += buttonSize + (8.0f * scale);
		const ImVec2 textSize = font->CalcTextSizeA(fontSize, 10000.0f, 0.0f, helper.label.c_str());
		drawList->AddText(font, fontSize, ImVec2(cursorX, centerY - textSize.y * 0.5f), textColor, helper.label.c_str());
		cursorX += textSize.x + itemSpacing;
	}
}

void Overlay::DrawStatus(ImDrawList *drawList, ImVec2 displaySize, float scale, float ease, float deltaTime) {
	UpdateBattery(deltaTime);

	std::time_t now = std::time(nullptr);
	std::tm *localTime = std::localtime(&now);
	char timeText[16] = "00:00";
	if (localTime) {
		std::strftime(timeText, sizeof(timeText), "%H:%M", localTime);
	}

	ImFont *font = ImGui::GetFont();
	const float barHeight = 50.0f * scale;
	const float topMargin = 32.0f * scale;
	const float sideMargin = 32.0f * scale;
	const float itemSpacing = 20.0f * scale;
	const float padding = 20.0f * scale;
	const float fontSize = ImGui::GetFontSize();

	float totalWidth = font->CalcTextSizeA(fontSize, 10000.0f, 0.0f, timeText).x + padding * 2.0f;
	if (psmReady_) {
		totalWidth += itemSpacing + (38.0f * scale);
	}

	const float barX = displaySize.x - totalWidth - sideMargin;
	const float barY = topMargin + ((1.0f - ease) * -20.0f * scale);
	const float centerY = barY + barHeight * 0.5f;
	float cursorX = barX + padding;
	const ImU32 textColor = IM_COL32(200, 200, 200, (int)(255.0f * ease));

	drawList->AddText(font, fontSize, ImVec2(cursorX, centerY - fontSize * 0.5f), textColor, timeText);
	cursorX += font->CalcTextSizeA(fontSize, 10000.0f, 0.0f, timeText).x;
	if (!psmReady_) {
		return;
	}

	cursorX += itemSpacing;
	const float bodyWidth = 32.0f * scale;
	const float bodyHeight = 20.0f * scale;
	const float tipWidth = 4.0f * scale;
	const float tipHeight = 10.0f * scale;
	const ImVec2 bodyMin(cursorX, centerY - bodyHeight * 0.5f);
	const ImVec2 bodyMax(bodyMin.x + bodyWidth, bodyMin.y + bodyHeight);
	drawList->AddRect(bodyMin, bodyMax, textColor, 3.0f * scale, 0, 2.0f * scale);
	drawList->AddRectFilled(
		ImVec2(bodyMax.x, bodyMin.y + (bodyHeight - tipHeight) * 0.5f),
		ImVec2(bodyMax.x + tipWidth, bodyMin.y + (bodyHeight + tipHeight) * 0.5f),
		textColor, 2.0f * scale, ImDrawFlags_RoundCornersRight);

	const float pct = std::clamp((float)batteryLevel_ / 100.0f, 0.0f, 1.0f);
	if (pct > 0.0f) {
		const float pad = 4.0f * scale;
		const ImVec2 fillMin(bodyMin.x + pad, bodyMin.y + pad);
		const ImVec2 fillMax(fillMin.x + (bodyWidth - pad * 2.0f) * pct, bodyMax.y - pad);
		const ImU32 fillColor = charging_ ? IM_COL32(255, 210, 90, (int)(255.0f * ease)) : IM_COL32(200, 200, 200, (int)(255.0f * ease));
		drawList->AddRectFilled(fillMin, fillMax, fillColor, 1.5f * scale);
	}

	if (charging_) {
		const ImVec2 center((bodyMin.x + bodyMax.x) * 0.5f, (bodyMin.y + bodyMax.y) * 0.5f);
		drawList->PathLineTo(ImVec2(center.x + 1.0f * scale, center.y - 6.0f * scale));
		drawList->PathLineTo(ImVec2(center.x - 3.0f * scale, center.y));
		drawList->PathLineTo(ImVec2(center.x + 0.5f * scale, center.y));
		drawList->PathLineTo(ImVec2(center.x - 1.5f * scale, center.y + 6.0f * scale));
		drawList->PathLineTo(ImVec2(center.x + 4.0f * scale, center.y - 1.0f * scale));
		drawList->PathLineTo(ImVec2(center.x, center.y - 1.0f * scale));
		drawList->PathFillConvex(IM_COL32(40, 40, 40, (int)(255.0f * ease)));
	}
}

void Overlay::DrawRAAlerts(Draw::DrawContext *draw, ImDrawList *drawList, ImVec2 displaySize, float scale, float deltaTime) {
	auto &notifications = RetroAchievements().Notifications();
	if (notifications.empty()) {
		return;
	}

	for (RANotification &notification : notifications) {
		notification.timer += deltaTime;
	}
	notifications.erase(std::remove_if(notifications.begin(), notifications.end(), [](const RANotification &notification) {
		return notification.timer >= notification.duration;
	}), notifications.end());
	if (notifications.empty()) {
		return;
	}

	const RAAlertPosition position = RetroAchievements().AlertPosition();
	const bool isTop = position == RAAlertPosition::TopLeft || position == RAAlertPosition::TopRight;
	const bool isRight = position == RAAlertPosition::TopRight || position == RAAlertPosition::BottomRight;
	const float margin = 16.0f * scale;
	const float spacing = 8.0f * scale;
	const float alertWidth = std::min(420.0f * scale, displaySize.x - margin * 2.0f);
	const float alertHeight = 100.0f * scale;
	const float padding = 12.0f * scale;
	const float cornerRadius = 14.0f * scale;
	const float badgeSize = 76.0f * scale;
	const float badgeRadius = 4.0f * scale;
	const float badgeMargin = 12.0f * scale;
	const float titleSize = ImGui::GetFontSize() * 0.85f;
	const float descriptionSize = ImGui::GetFontSize() * 0.65f;
	ImFont *font = ImGui::GetFont();
	ImFont *descriptionFont = font;
	if (ImGui::GetIO().Fonts->Fonts.Size > 1) {
		descriptionFont = ImGui::GetIO().Fonts->Fonts[1];
	}

	size_t visibleIndex = 0;
	for (size_t i = 0; i < notifications.size(); ++i) {
		RANotification &notification = notifications[i];
		if (notification.timer < 0.0f) {
			continue;
		}

		float slideProgress = 1.0f;
		if (notification.timer < notification.slideIn) {
			slideProgress = EaseOutCubic(notification.timer / notification.slideIn);
		} else if (notification.timer > notification.duration - notification.slideOut) {
			slideProgress = EaseOutCubic((notification.duration - notification.timer) / notification.slideOut);
		}
		slideProgress = std::clamp(slideProgress, 0.0f, 1.0f);
		const int alpha = (int)(230.0f * slideProgress);
		if (alpha <= 0) {
			continue;
		}

		const float stack = (alertHeight + spacing) * (float)visibleIndex;
		const float anchorX = isRight ? displaySize.x - alertWidth - margin : margin;
		const float anchorY = isTop ? margin + stack : displaySize.y - margin - alertHeight - stack;
		const float slideOffsetY = isTop
			? -(alertHeight + margin + stack) * (1.0f - slideProgress)
			: (alertHeight + margin + stack) * (1.0f - slideProgress);
		const ImVec2 min(anchorX, anchorY + slideOffsetY);
		const ImVec2 max(anchorX + alertWidth, min.y + alertHeight);

		drawList->AddRectFilled(min, max, IM_COL32(35, 35, 40, alpha), cornerRadius);
		drawList->AddRect(min, max, IM_COL32(70, 70, 80, (int)(180.0f * slideProgress)), cornerRadius, 0, 1.5f * scale);

		Draw::Texture *badgeTexture = nullptr;
		const bool isRAIcon = notification.badgeName == "ra_icon";
		if (isRAIcon) {
			badgeTexture = LoadRAIconTexture(draw);
		} else {
			badgeTexture = RetroAchievements().GetBadgeTexture(draw, notification.badgeName);
		}

		float textX = min.x + padding;
		const ImVec2 badgeMin(min.x + badgeMargin, min.y + (alertHeight - badgeSize) * 0.5f);
		const ImVec2 badgeMax(badgeMin.x + badgeSize, badgeMin.y + badgeSize);
		if (badgeTexture) {
			float drawBadgeSize = badgeSize;
			float drawBadgeX = badgeMin.x;
			float drawBadgeY = badgeMin.y;
			if (isRAIcon) {
				drawBadgeSize = badgeSize * 0.70f;
				drawBadgeX += (badgeSize - drawBadgeSize) * 0.5f;
				drawBadgeY += (badgeSize - drawBadgeSize) * 0.5f;
			}
			const ImTextureID textureId = ImGui_ImplThin3d_AddTextureTemp(badgeTexture);
			drawList->AddImageRounded(textureId, ImVec2(drawBadgeX, drawBadgeY),
				ImVec2(drawBadgeX + drawBadgeSize, drawBadgeY + drawBadgeSize),
				ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, alpha), badgeRadius);
			textX = badgeMin.x + badgeSize + badgeMargin;
		} else {
			drawList->AddRectFilled(badgeMin, badgeMax, IM_COL32(45, 45, 52, alpha), badgeRadius);
			const char *ra = "RA";
			const ImVec2 raSize = font->CalcTextSizeA(titleSize, 10000.0f, 0.0f, ra);
			drawList->AddText(font, titleSize, ImVec2((badgeMin.x + badgeMax.x - raSize.x) * 0.5f,
				(badgeMin.y + badgeMax.y - raSize.y) * 0.5f), IM_COL32(230, 230, 238, alpha), ra);
			textX = badgeMin.x + badgeSize + badgeMargin;
		}

		const float maxTextWidth = max.x - textX - padding;
		std::string title = TruncateToWidth(font, titleSize, notification.title, maxTextWidth);
		std::string description = notification.description;
		const float maxDescriptionHeight = descriptionSize * 2.5f;
		if (descriptionFont->CalcTextSizeA(descriptionSize, 10000.0f, maxTextWidth, description.c_str()).y > maxDescriptionHeight) {
			description += "...";
			while (description.size() > 4 &&
				descriptionFont->CalcTextSizeA(descriptionSize, 10000.0f, maxTextWidth, description.c_str()).y > maxDescriptionHeight) {
				description.erase(description.size() - 4, 1);
			}
		}

		const ImVec2 titleTextSize = font->CalcTextSizeA(titleSize, 10000.0f, 0.0f, title.c_str());
		const ImVec2 descriptionTextSize = descriptionFont->CalcTextSizeA(descriptionSize, 10000.0f, maxTextWidth, description.c_str());
		const float textSpacing = 4.0f * scale;
		const float totalTextHeight = titleTextSize.y + textSpacing + descriptionTextSize.y;
		const float titleY = min.y + (alertHeight - totalTextHeight) * 0.5f;
		const float descriptionY = titleY + titleTextSize.y + textSpacing;
		const ImU32 titleColor = IM_COL32(255, 255, 255, alpha);
		const ImU32 descriptionColor = IM_COL32(185, 185, 195, alpha);

		drawList->AddText(font, titleSize, ImVec2(textX + 1.0f, titleY + 1.0f),
			IM_COL32(0, 0, 0, (int)(80.0f * slideProgress)), title.c_str());
		drawList->AddText(font, titleSize, ImVec2(textX, titleY), titleColor, title.c_str());
		drawList->AddText(descriptionFont, descriptionSize, ImVec2(textX, descriptionY), descriptionColor,
			description.c_str(), nullptr, maxTextWidth);
		visibleIndex++;
	}
}

void Overlay::DrawUI(float width, float height, float deltaTime) {
	animTimer_ = std::min(animTimer_ + deltaTime, kOverlayAnimDuration);
	if (cheatsLoading_) {
		loaderTimer_ += deltaTime;
	}
	const float ease = EaseOutCubic(animTimer_ / kOverlayAnimDuration);
	ImDrawList *drawList = ImGui::GetForegroundDrawList();
	const ImVec2 displaySize(width, height);
	const float scale = std::max(1.0f, height / 720.0f);

	DrawBackground(drawList, displaySize, ease);
	DrawTitle(drawList, displaySize, scale, ease);
	DrawMenu(drawList, displaySize, scale, ease);
	DrawHelpers(drawList, displaySize, scale, ease);
	DrawSocialArea(drawList, displaySize, scale, ease);
	DrawStatus(drawList, displaySize, scale, ease, deltaTime);
}

void Overlay::Render(Draw::DrawContext *draw) {
	if (!ready_ || !context_ || !draw) {
		return;
	}

	const bool hasRAAlerts = !RetroAchievements().Notifications().empty();
	if (!visible_ && !hasRAAlerts) {
		return;
	}

	ImGui::SetCurrentContext(context_);
	const float width = (float)std::max(1, g_display.pixel_xres);
	const float height = (float)std::max(1, g_display.pixel_yres);
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(width, height);
	io.DeltaTime = 1.0f / std::max(1.0f, g_display.display_hz);

	const float orthoW = g_display.dp_xres > 0 ? (float)g_display.dp_xres : width;
	const float orthoH = g_display.dp_yres > 0 ? (float)g_display.dp_yres : height;
	const float scale = std::max(1.0f, height / 720.0f);
	const float loadedFontSize = io.Fonts->Fonts.Size > 0 ? io.Fonts->Fonts[0]->FontSize : 21.0f;
	if (loadedFontSize > 0.0f) {
		io.FontGlobalScale = (Display::FontSize * scale) / loadedFontSize;
	}

	ImGui_ImplThin3d_NewFrame(draw, ComputeOrthoMatrix(orthoW, orthoH, draw->GetDeviceCaps().coordConvention));
	ImGui::NewFrame();
	if (visible_) {
		if (cheatsLoading_) {
			LoadLoaderTexture(draw);
		}
		DrawUI(width, height, io.DeltaTime);
	}
	DrawRAAlerts(draw, ImGui::GetForegroundDrawList(), ImVec2(width, height), scale, io.DeltaTime);
	ImGui::Render();

	const Draw::RenderPassInfo overlayPass{
		Draw::RPAction::KEEP,
		Draw::RPAction::KEEP,
		Draw::RPAction::KEEP,
		0x00000000,
		1.0f,
		0,
		"TicoOverlay",
	};
	draw->BindFramebufferAsRenderTarget(nullptr, overlayPass, "TicoOverlay");
	ImGui_ImplThin3d_RenderDrawData(ImGui::GetDrawData(), draw);
}

}  // namespace Tico
