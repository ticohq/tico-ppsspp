#include "PpssppRuntime.h"

#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(SWITCH)

#if !defined(PPSSPP_SWITCH_VULKAN_ONLY)
#error "Tico PPSSPP runtime is Vulkan-only. Build with PPSSPP_SWITCH_VULKAN_ONLY."
#endif

#include "tico/TicoGraphicsHost.h"
#include "tico/PpssppTicoConfig.h"
#include "tico/TicoAssetInstaller.h"
#include "tico/TicoAudioSfx.h"
#include "tico/TicoOverlay.h"
#include "tico/TicoRetroAchievements.h"

#include "Common/CPUDetect.h"
#include "Common/File/FileUtil.h"
#include "Common/File/VFS/DirectoryReader.h"
#include "Common/File/VFS/VFS.h"
#include "Common/GPU/thin3d.h"
#include "Common/GraphicsContext.h"
#include "Common/Log/LogManager.h"
#include "Common/Profiler/Profiler.h"
#include "Common/StringUtils.h"
#include "Common/System/Display.h"
#include "Common/System/Request.h"
#include "Common/System/System.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreParameter.h"
#include "Core/CoreTiming.h"
#include "Core/CwCheat.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/FrameTiming.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceDisplay.h"
#include "Core/HW/StereoResampler.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "Core/Util/PathUtil.h"
#include "GPU/GPUCommon.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Tico {
namespace {

constexpr int kAudioSampleRate = 48000;
constexpr int kAudioSamples = 1024;
constexpr int kAudioChannels = 2;
constexpr int kAudioBytesPerSample = sizeof(s16);
constexpr int kAudioBufferCount = 4;
constexpr size_t kAudioDataBytes = kAudioSamples * kAudioChannels * kAudioBytesPerSample;
constexpr size_t kAudioBufferBytes = (kAudioDataBytes + 0xFFF) & ~(size_t)0xFFF;
constexpr size_t kAudioThreadStackSize = 0x10000;
constexpr int kAudioThreadPriority = 0x28;
constexpr int kAudioThreadCore = 1;
constexpr size_t kCheatLoadThreadStackSize = 0x20000;
constexpr int kCheatLoadThreadPriority = 0x3B;
constexpr int kCheatLoadThreadCore = -2;
constexpr int kDefaultRightStickButtonThreshold = 12000;
constexpr size_t kCheatMetadataLineChars = 68;

enum class RightStickMode {
	Disabled,
	FaceButtons,
	DPad,
	Analog,
};

struct InputButtonMapping {
	u64 switchButton = 0;
	const char *configKey = nullptr;
	u32 defaultPspButtons = 0;
	u32 pspButtons = 0;
};

struct InputConfig {
	std::array<InputButtonMapping, 16> buttonMappings{};
	RightStickMode rightStickMode = RightStickMode::FaceButtons;
	int rightStickButtonThreshold = kDefaultRightStickButtonThreshold;
};

struct RuntimeState {
	LogCallback log;
	bool running = true;
	bool booted = false;
	bool frameOpen = false;
	bool hostFrameOpen = false;
	bool ppssppShutdown = false;
	bool chainloadLauncher = false;
	std::string contentPath;
	TicoGraphicsHost graphicsHost;
	GraphicsContext *graphicsContext = nullptr;
	Draw::DrawContext *draw = nullptr;
	Overlay overlay;
	DisplaySettings displaySettings;
	bool displaySettingsLoaded = false;
	std::mutex mainThreadMutex;
	std::vector<std::function<void()>> mainThreadQueue;
	StereoResampler resampler;
	std::array<bool, Ppsspp::SaveStateSlotCount> saveStateSlots{};
	u64 lastSaveStateScanMs = 0;
	u32 lastPspButtons = 0;
	InputConfig inputConfig;
	bool audioReady = false;
	int audioSampleRate = kAudioSampleRate;
	int audioBufferSamples = kAudioSamples;
	AudioOutBuffer audioBuffers[kAudioBufferCount]{};
	u8 *audioBufferData[kAudioBufferCount]{};
	Thread audioThread{};
	std::atomic<bool> audioThreadStop{false};
	bool audioThreadCreated = false;
	Thread cheatLoadThread{};
	std::atomic<bool> cheatLoadThreadActive{false};
	bool cheatLoadThreadCreated = false;
	int frameCount = 0;
};

RuntimeState g_state;

InputConfig DefaultInputConfig() {
	InputConfig config{};
	config.buttonMappings = {{
		{ HidNpadButton_B, "ppsspp_map_b", CTRL_CROSS, CTRL_CROSS },
		{ HidNpadButton_A, "ppsspp_map_a", CTRL_CIRCLE, CTRL_CIRCLE },
		{ HidNpadButton_Y, "ppsspp_map_y", CTRL_SQUARE, CTRL_SQUARE },
		{ HidNpadButton_X, "ppsspp_map_x", CTRL_TRIANGLE, CTRL_TRIANGLE },
		{ HidNpadButton_Up, "ppsspp_map_dpad_up", CTRL_UP, CTRL_UP },
		{ HidNpadButton_Down, "ppsspp_map_dpad_down", CTRL_DOWN, CTRL_DOWN },
		{ HidNpadButton_Left, "ppsspp_map_dpad_left", CTRL_LEFT, CTRL_LEFT },
		{ HidNpadButton_Right, "ppsspp_map_dpad_right", CTRL_RIGHT, CTRL_RIGHT },
		{ HidNpadButton_Plus, "ppsspp_map_plus", CTRL_START, CTRL_START },
		{ HidNpadButton_Minus, "ppsspp_map_minus", CTRL_SELECT, CTRL_SELECT },
		{ HidNpadButton_L, "ppsspp_map_l", CTRL_LTRIGGER, CTRL_LTRIGGER },
		{ HidNpadButton_R, "ppsspp_map_r", CTRL_RTRIGGER, CTRL_RTRIGGER },
		{ HidNpadButton_ZL, "ppsspp_map_zl", CTRL_L2, CTRL_L2 },
		{ HidNpadButton_ZR, "ppsspp_map_zr", CTRL_R2, CTRL_R2 },
		{ HidNpadButton_StickL, "ppsspp_map_stick_l", CTRL_L3, CTRL_L3 },
		{ HidNpadButton_StickR, "ppsspp_map_stick_r", CTRL_R3, CTRL_R3 },
	}};
	return config;
}

void Log(const char *fmt, ...) {
	if (!g_state.log || !fmt) {
		return;
	}

	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	g_state.log(buffer);
}

void DrainMainThreadQueue() {
	std::vector<std::function<void()>> work;
	{
		std::lock_guard<std::mutex> lock(g_state.mainThreadMutex);
		work.swap(g_state.mainThreadQueue);
	}
	for (auto &fn : work) {
		fn();
	}
}

void QueueMainThread(std::function<void()> func) {
	std::lock_guard<std::mutex> lock(g_state.mainThreadMutex);
	g_state.mainThreadQueue.push_back(std::move(func));
}

float ClampAnalog(float value) {
	return std::clamp(value, -1.0f, 1.0f);
}

float NormalizeStickAxis(int value) {
	float normalized = ClampAnalog(value / 32767.0f);
	if (std::fabs(normalized) < g_Config.fAnalogDeadzone) {
		return 0.0f;
	}
	return ClampAnalog(normalized * g_Config.fAnalogSensitivity);
}

std::string NormalizeInputConfigValue(const std::string &value) {
	std::string normalized;
	normalized.reserve(value.size());
	for (char ch : value) {
		if (std::isalnum((unsigned char)ch)) {
			normalized.push_back((char)std::tolower((unsigned char)ch));
		}
	}
	return normalized;
}

bool PspButtonsFromName(const std::string &name, u32 *buttons) {
	if (!buttons) {
		return false;
	}

	const std::string normalized = NormalizeInputConfigValue(name);
	if (normalized.empty() || normalized == "none" || normalized == "disabled" || normalized == "off") {
		*buttons = 0;
		return true;
	}
	if (normalized == "cross" || normalized == "x") {
		*buttons = CTRL_CROSS;
	} else if (normalized == "circle" || normalized == "o") {
		*buttons = CTRL_CIRCLE;
	} else if (normalized == "square") {
		*buttons = CTRL_SQUARE;
	} else if (normalized == "triangle") {
		*buttons = CTRL_TRIANGLE;
	} else if (normalized == "up" || normalized == "dpadup") {
		*buttons = CTRL_UP;
	} else if (normalized == "down" || normalized == "dpaddown") {
		*buttons = CTRL_DOWN;
	} else if (normalized == "left" || normalized == "dpadleft") {
		*buttons = CTRL_LEFT;
	} else if (normalized == "right" || normalized == "dpadright") {
		*buttons = CTRL_RIGHT;
	} else if (normalized == "start") {
		*buttons = CTRL_START;
	} else if (normalized == "select") {
		*buttons = CTRL_SELECT;
	} else if (normalized == "l" || normalized == "l1" || normalized == "lefttrigger") {
		*buttons = CTRL_LTRIGGER;
	} else if (normalized == "r" || normalized == "r1" || normalized == "righttrigger") {
		*buttons = CTRL_RTRIGGER;
	} else if (normalized == "l2") {
		*buttons = CTRL_L2;
	} else if (normalized == "r2") {
		*buttons = CTRL_R2;
	} else if (normalized == "l3") {
		*buttons = CTRL_L3;
	} else if (normalized == "r3") {
		*buttons = CTRL_R3;
	} else {
		return false;
	}
	return true;
}

u32 ParsePspButtonMapping(const std::string &value, u32 fallback) {
	u32 parsedButtons = 0;
	bool parsedAny = false;
	size_t tokenStart = 0;

	for (size_t i = 0; i <= value.size(); ++i) {
		if (i != value.size() && value[i] != '+' && value[i] != ',' && value[i] != '|') {
			continue;
		}

		const std::string token = value.substr(tokenStart, i - tokenStart);
		tokenStart = i + 1;
		if (NormalizeInputConfigValue(token).empty()) {
			continue;
		}

		u32 tokenButtons = 0;
		if (!PspButtonsFromName(token, &tokenButtons)) {
			return fallback;
		}
		parsedButtons |= tokenButtons;
		parsedAny = true;
	}

	return parsedAny ? parsedButtons : fallback;
}

RightStickMode ParseRightStickMode(const std::string &value, RightStickMode fallback) {
	const std::string normalized = NormalizeInputConfigValue(value);
	if (normalized == "disabled" || normalized == "none" || normalized == "off") {
		return RightStickMode::Disabled;
	}
	if (normalized == "facebuttons" || normalized == "face" || normalized == "buttons") {
		return RightStickMode::FaceButtons;
	}
	if (normalized == "dpad" || normalized == "directionalpad") {
		return RightStickMode::DPad;
	}
	if (normalized == "analog" || normalized == "rightanalog" || normalized == "stick") {
		return RightStickMode::Analog;
	}
	return fallback;
}

InputConfig LoadInputConfig(const CoreConfig &coreConfig) {
	InputConfig inputConfig = DefaultInputConfig();
	const auto &options = coreConfig.Options();

	for (InputButtonMapping &mapping : inputConfig.buttonMappings) {
		if (const std::string *value = FindOption(options, mapping.configKey)) {
			mapping.pspButtons = ParsePspButtonMapping(*value, mapping.defaultPspButtons);
		}
	}
	if (const std::string *value = FindOption(options, "ppsspp_right_stick_mode")) {
		inputConfig.rightStickMode = ParseRightStickMode(*value, inputConfig.rightStickMode);
	}
	if (const std::string *value = FindOption(options, "ppsspp_right_stick_threshold")) {
		inputConfig.rightStickButtonThreshold = std::clamp(OptionInt(*value, inputConfig.rightStickButtonThreshold), 1, 32767);
	}

	return inputConfig;
}

void AddRightStickButtons(const FrameInput &input, u32 *buttons) {
	if (!buttons) {
		return;
	}
	if (g_state.inputConfig.rightStickMode == RightStickMode::Disabled || g_state.inputConfig.rightStickMode == RightStickMode::Analog) {
		return;
	}

	const int threshold = g_state.inputConfig.rightStickButtonThreshold;
	if (g_state.inputConfig.rightStickMode == RightStickMode::DPad) {
		if (input.rightStickY > threshold) {
			*buttons |= CTRL_UP;
		}
		if (input.rightStickY < -threshold) {
			*buttons |= CTRL_DOWN;
		}
		if (input.rightStickX < -threshold) {
			*buttons |= CTRL_LEFT;
		}
		if (input.rightStickX > threshold) {
			*buttons |= CTRL_RIGHT;
		}
		return;
	}

	if (input.rightStickY > threshold) {
		*buttons |= CTRL_TRIANGLE;
	}
	if (input.rightStickY < -threshold) {
		*buttons |= CTRL_CROSS;
	}
	if (input.rightStickX < -threshold) {
		*buttons |= CTRL_SQUARE;
	}
	if (input.rightStickX > threshold) {
		*buttons |= CTRL_CIRCLE;
	}
}

void ClearPspInput() {
	if (g_state.lastPspButtons != 0) {
		__CtrlUpdateButtons(0, g_state.lastPspButtons);
		g_state.lastPspButtons = 0;
	}
	__CtrlSetAnalogXY(CTRL_STICK_LEFT, 0.0f, 0.0f);
	__CtrlSetAnalogXY(CTRL_STICK_RIGHT, 0.0f, 0.0f);
}

void UpdatePspInput(const FrameInput &input) {
	u32 currentButtons = 0;
	const u64 buttons = input.buttons;
	for (const InputButtonMapping &mapping : g_state.inputConfig.buttonMappings) {
		if (buttons & mapping.switchButton) {
			currentButtons |= mapping.pspButtons;
		}
	}
	AddRightStickButtons(input, &currentButtons);

	__CtrlUpdateButtons(currentButtons & ~g_state.lastPspButtons, g_state.lastPspButtons & ~currentButtons);
	g_state.lastPspButtons = currentButtons;
	__CtrlSetAnalogXY(CTRL_STICK_LEFT, NormalizeStickAxis(input.leftStickX), NormalizeStickAxis(input.leftStickY));
	if (g_state.inputConfig.rightStickMode == RightStickMode::Analog) {
		__CtrlSetAnalogXY(CTRL_STICK_RIGHT, NormalizeStickAxis(input.rightStickX), NormalizeStickAxis(input.rightStickY));
	} else {
		__CtrlSetAnalogXY(CTRL_STICK_RIGHT, 0.0f, 0.0f);
	}
}

void FreeAudioBuffers() {
	for (int i = 0; i < kAudioBufferCount; ++i) {
		free(g_state.audioBufferData[i]);
		g_state.audioBufferData[i] = nullptr;
		g_state.audioBuffers[i] = {};
	}
}

void FillAudioBuffer(AudioOutBuffer *buffer) {
	if (!buffer || !buffer->buffer) {
		return;
	}

	if (g_state.audioReady) {
		g_state.resampler.Mix(reinterpret_cast<s16 *>(buffer->buffer), kAudioSamples, false, g_state.audioSampleRate);
	} else {
		std::memset(buffer->buffer, 0, kAudioDataBytes);
	}
	TrophySfx().Mix(reinterpret_cast<s16 *>(buffer->buffer), kAudioSamples, g_state.audioSampleRate);
	buffer->data_offset = 0;
	buffer->data_size = kAudioDataBytes;
	armDCacheClean(buffer->buffer, buffer->buffer_size);
}

void AudioThreadEntry(void *) {
	SetCurrentThreadName("TicoAudio");
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, kAudioThreadCore, 1ULL << kAudioThreadCore);

	while (!g_state.audioThreadStop.load(std::memory_order_acquire)) {
		AudioOutBuffer *releasedBuffer = nullptr;
		u32 releasedCount = 0;
		Result rc = audoutWaitPlayFinish(&releasedBuffer, &releasedCount, 100000000ULL);
		if (g_state.audioThreadStop.load(std::memory_order_acquire)) {
			break;
		}
		if (R_FAILED(rc) || releasedCount == 0 || !releasedBuffer) {
			continue;
		}

		FillAudioBuffer(releasedBuffer);
		rc = audoutAppendAudioOutBuffer(releasedBuffer);
		if (R_FAILED(rc)) {
			break;
		}
	}
}

bool InitAudio() {
	g_state.audioReady = false;
	g_state.audioSampleRate = kAudioSampleRate;
	g_state.audioBufferSamples = kAudioSamples;

	Result rc = audoutInitialize();
	if (R_FAILED(rc)) {
		Log("audoutInitialize failed rc=0x%x", (unsigned)rc);
		return false;
	}

	const u32 sampleRate = audoutGetSampleRate();
	const u32 channelCount = audoutGetChannelCount();
	const PcmFormat pcmFormat = audoutGetPcmFormat();
	Log("audout device rate=%u channels=0x%x format=%d state=%d",
		(unsigned)sampleRate, (unsigned)channelCount, (int)pcmFormat, (int)audoutGetDeviceState());
	if (sampleRate == 0 || pcmFormat != PcmFormat_Int16) {
		Log("audout unsupported format");
		audoutExit();
		return false;
	}
	g_state.audioSampleRate = (int)sampleRate;

	for (int i = 0; i < kAudioBufferCount; ++i) {
		g_state.audioBufferData[i] = static_cast<u8 *>(memalign(0x1000, kAudioBufferBytes));
		if (!g_state.audioBufferData[i]) {
			Log("audout buffer allocation failed index=%d", i);
			FreeAudioBuffers();
			audoutExit();
			return false;
		}
		std::memset(g_state.audioBufferData[i], 0, kAudioBufferBytes);
		g_state.audioBuffers[i].next = nullptr;
		g_state.audioBuffers[i].buffer = g_state.audioBufferData[i];
		g_state.audioBuffers[i].buffer_size = kAudioBufferBytes;
		g_state.audioBuffers[i].data_size = kAudioDataBytes;
		g_state.audioBuffers[i].data_offset = 0;
		armDCacheClean(g_state.audioBuffers[i].buffer, g_state.audioBuffers[i].buffer_size);
	}

	rc = audoutStartAudioOut();
	if (R_FAILED(rc)) {
		Log("audoutStartAudioOut failed rc=0x%x", (unsigned)rc);
		FreeAudioBuffers();
		audoutExit();
		return false;
	}

	for (int i = 0; i < kAudioBufferCount; ++i) {
		rc = audoutAppendAudioOutBuffer(&g_state.audioBuffers[i]);
		if (R_FAILED(rc)) {
			Log("audoutAppendAudioOutBuffer failed index=%d rc=0x%x", i, (unsigned)rc);
			audoutStopAudioOut();
			FreeAudioBuffers();
			audoutExit();
			return false;
		}
	}

	g_state.audioThreadStop.store(false, std::memory_order_release);
	rc = threadCreate(&g_state.audioThread, AudioThreadEntry, nullptr, nullptr, kAudioThreadStackSize, kAudioThreadPriority, kAudioThreadCore);
	if (R_FAILED(rc)) {
		Log("audio threadCreate failed rc=0x%x", (unsigned)rc);
		audoutStopAudioOut();
		FreeAudioBuffers();
		audoutExit();
		return false;
	}
	g_state.audioThreadCreated = true;

	rc = threadStart(&g_state.audioThread);
	if (R_FAILED(rc)) {
		Log("audio threadStart failed rc=0x%x", (unsigned)rc);
		threadClose(&g_state.audioThread);
		g_state.audioThreadCreated = false;
		audoutStopAudioOut();
		FreeAudioBuffers();
		audoutExit();
		return false;
	}

	g_state.audioReady = true;
	g_state.audioBufferSamples = kAudioSamples;
	return true;
}

void ShutdownAudio() {
	g_state.audioReady = false;
	g_state.audioThreadStop.store(true, std::memory_order_release);
	if (g_state.audioThreadCreated) {
		audoutStopAudioOut();
		threadWaitForExit(&g_state.audioThread);
		threadClose(&g_state.audioThread);
		g_state.audioThread = {};
		g_state.audioThreadCreated = false;
	}
	audoutExit();
	FreeAudioBuffers();
	g_state.audioSampleRate = kAudioSampleRate;
	g_state.audioBufferSamples = kAudioSamples;
}

void UpdateDisplayMode() {
	static AppletOperationMode lastMode = (AppletOperationMode)-1;
	const AppletOperationMode mode = appletGetOperationMode();
	if (mode != lastMode) {
		if (mode == AppletOperationMode_Handheld) {
			const Result dimRc = nwindowSetDimensions(nwindowGetDefault(), 1280, 720);
			const Result cropRc = nwindowSetCrop(nwindowGetDefault(), 0, 0, 1280, 720);
			g_display.Recalculate(1280, 720, 1.0f, 1.0f, 1.0f);
			Log("display mode=handheld dim=0x%x crop=0x%x", (unsigned)dimRc, (unsigned)cropRc);
		} else {
			const Result dimRc = nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);
			const Result cropRc = nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
			g_display.Recalculate(1920, 1080, 1.0f, 1.0f, 1.0f);
			Log("display mode=docked dim=0x%x crop=0x%x", (unsigned)dimRc, (unsigned)cropRc);
		}
		g_display.display_hz = 60.0f;
		lastMode = mode;
		if (g_state.displaySettingsLoaded) {
			g_state.displaySettings = LoadPpssppDisplaySettings(g_state.log);
			if (g_state.displaySettings.mode == DisplayMode::Integer) {
				g_state.displaySettings.size = DisplaySize::Auto;
			}
			SavePpssppDisplaySettings(g_state.displaySettings, g_state.log);
			ApplyPpssppDisplaySettings(g_state.displaySettings);
			g_state.overlay.ReloadDisplaySettings();
		}
	}

	if (PSP_GetBootState() == BootState::Complete) {
		PSP_CoreParameter().pixelWidth = g_display.pixel_xres;
		PSP_CoreParameter().pixelHeight = g_display.pixel_yres;
	}
}

void InitializeConfig() {
	g_Config.RestoreDefaults(RestoreSettingsBits::SETTINGS | RestoreSettingsBits::CONTROLS, true);
	PpssppCoreConfig config(g_state.log);
	config.Load();
	g_state.inputConfig = LoadInputConfig(config.RawConfig());
	config.Apply(g_state.audioReady);
	g_state.displaySettings = LoadPpssppDisplaySettings(g_state.log);
	g_state.displaySettingsLoaded = true;
	SavePpssppDisplaySettings(g_state.displaySettings, g_state.log);
	ApplyPpssppDisplaySettings(g_state.displaySettings);
	CreateSysDirectories();
	g_VFS.Register("", new DirectoryReader(::Path(Paths::PpssppDataRoot)));
	UpdateUIState(UISTATE_INGAME);
}

char *ReadCheatDbLine(char *buffer, int size, FILE *fp) {
	char *line = fgets(buffer, size, fp);
	if (!line) {
		return nullptr;
	}

	size_t length = strlen(line);
	while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
		line[--length] = '\0';
	}
	return line;
}

Path GetCheatDbPath() {
	const Path cheatDir = GetSysDirectory(DIRECTORY_CHEATS);
	const Path cheatDb = cheatDir / "cheat.db";
	if (File::Exists(cheatDb)) {
		return cheatDb;
	}

	const Path cheatsDb = cheatDir / "cheats.db";
	if (File::Exists(cheatsDb)) {
		return cheatsDb;
	}
	return Path();
}

bool CheatNameExists(const std::vector<CheatFileInfo> &fileInfo, const std::string &name) {
	for (const CheatFileInfo &existing : fileInfo) {
		if (existing.name == name) {
			return true;
		}
	}
	return false;
}

bool ImportCheatsFromDb(const Path &cheatDbPath) {
	const std::string gameID = g_paramSFO.GetDiscID();
	if (gameID.length() != 9) {
		Log("tico cheats import skipped invalid game id=%s", gameID.c_str());
		return false;
	}

	CWCheatEngine engine(gameID);
	engine.CreateCheatFile();
	const std::vector<CheatFileInfo> fileInfo = engine.FileInfo();
	const bool hasExistingCheats = !fileInfo.empty();
	const std::string dbGameID = StringFromFormat("_S %s-%s", gameID.substr(0, 4).c_str(), gameID.substr(4).c_str());

	FILE *in = File::OpenCFile(cheatDbPath, "rt");
	if (!in) {
		Log("tico cheats db open failed path=%s", cheatDbPath.ToString().c_str());
		return hasExistingCheats;
	}

	std::vector<std::string> title;
	std::vector<std::string> newList;
	char lineBuffer[2048]{};
	bool parseGameEntry = false;
	bool parseCheatEntry = false;

	while (!feof(in)) {
		char *line = ReadCheatDbLine(lineBuffer, sizeof(lineBuffer), in);
		if (!line || line[0] == '\0') {
			continue;
		}

		if (line[0] == '_' && line[1] == 'S') {
			parseGameEntry = dbGameID == line;
			parseCheatEntry = false;
		} else if (parseGameEntry && line[0] == '_' && line[1] == 'C') {
			parseCheatEntry = !CheatNameExists(fileInfo, std::string(line).substr(4));
		}

		if (!parseGameEntry) {
			if (!newList.empty()) {
				break;
			}
			continue;
		}

		if (line[0] == '_' && (line[1] == 'S' || line[1] == 'G') && title.size() < 2) {
			title.push_back(line);
		} else if (parseCheatEntry && ((line[0] == '_' && (line[1] == 'C' || line[1] == 'L')) || line[0] == '/' || line[0] == '#')) {
			newList.push_back(line);
		}
	}
	fclose(in);

	if (newList.empty()) {
		Log("tico cheats import no new entries path=%s game=%s existing=%u",
			cheatDbPath.ToString().c_str(), gameID.c_str(), (unsigned)fileInfo.size());
		return hasExistingCheats;
	}

	std::string firstCheatLine;
	FILE *existingFile = File::OpenCFile(engine.CheatFilename(), "rt");
	if (existingFile) {
		char temp[2048]{};
		char *line = ReadCheatDbLine(temp, sizeof(temp), existingFile);
		if (line) {
			firstCheatLine = line;
		}
		fclose(existingFile);
	}

	if (firstCheatLine.empty() || firstCheatLine[0] != '_' || firstCheatLine[1] != 'S') {
		for (int i = (int)title.size(); i > 0; --i) {
			newList.insert(newList.begin(), title[i - 1]);
		}
	}

	FILE *append = File::OpenCFile(engine.CheatFilename(), "at");
	if (!append) {
		Log("tico cheats file append failed path=%s", engine.CheatFilename().ToString().c_str());
		return false;
	}

	fputc('\n', append);
	for (int i = 0; i < (int)newList.size(); ++i) {
		fprintf(append, "%s", newList[i].c_str());
		if (i < (int)newList.size() - 1) {
			fputc('\n', append);
		}
	}
	fclose(append);

	Log("tico cheats imported lines=%u db=%s game=%s file=%s",
		(unsigned)newList.size(), cheatDbPath.ToString().c_str(), gameID.c_str(), engine.CheatFilename().ToString().c_str());
	return true;
}

std::string TrimAscii(std::string_view text) {
	size_t begin = 0;
	size_t end = text.size();
	while (begin < end && std::isspace((unsigned char)text[begin])) {
		begin++;
	}
	while (end > begin && std::isspace((unsigned char)text[end - 1])) {
		end--;
	}
	return std::string(text.substr(begin, end - begin));
}

void TrimCheatDecorators(std::string *text) {
	if (!text) {
		return;
	}

	*text = TrimAscii(*text);
	while (text->size() >= 3 && text->compare(0, 3, ">>>") == 0) {
		text->erase(0, 3);
		*text = TrimAscii(*text);
	}
	while (text->size() >= 3 && text->compare(text->size() - 3, 3, "<<<") == 0) {
		text->erase(text->size() - 3);
		*text = TrimAscii(*text);
	}

	const auto isDecorator = [](char ch) {
		return ch == '_' || ch == '[' || ch == ']' || ch == '<' || ch == '>' ||
			ch == '#' || ch == '^' || ch == 'v' || ch == 'V' || ch == '?';
	};
	while (!text->empty() && isDecorator(text->front())) {
		text->erase(text->begin());
		*text = TrimAscii(*text);
	}
	while (!text->empty() && isDecorator(text->back())) {
		text->pop_back();
		*text = TrimAscii(*text);
	}
}

std::vector<std::string> SplitCheatMetadataLines(const std::string &text) {
	std::vector<std::string> lines;
	std::string remaining = TrimAscii(text);
	while (remaining.size() > kCheatMetadataLineChars) {
		size_t split = remaining.rfind(' ', kCheatMetadataLineChars);
		if (split == std::string::npos || split < kCheatMetadataLineChars / 2) {
			split = remaining.find(' ', kCheatMetadataLineChars);
		}
		if (split == std::string::npos) {
			split = kCheatMetadataLineChars;
		}

		std::string line = TrimAscii(std::string_view(remaining).substr(0, split));
		if (!line.empty()) {
			lines.push_back(std::move(line));
		}
		const bool splitAtSpace = split < remaining.size() && remaining[split] == ' ';
		const size_t next = split + (splitAtSpace ? 1 : 0);
		remaining = next < remaining.size() ? TrimAscii(std::string_view(remaining).substr(next)) : "";
	}
	if (!remaining.empty()) {
		lines.push_back(std::move(remaining));
	}
	if (lines.empty() && !text.empty()) {
		lines.push_back(text);
	}
	return lines;
}

CheatMenuEntry MakeCheatMenuEntry(const CheatFileInfo &info, int sourceIndex) {
	CheatMenuEntry entry;
	entry.name = info.name;
	entry.enabled = info.enabled;
	entry.toggleable = true;
	entry.sourceIndex = sourceIndex;

	std::string label = TrimAscii(info.name);
	const size_t tipStart = label.find("[#");
	const bool arrowTip = label.size() >= 2 &&
		(label[0] == '^' || label[0] == 'v' || label[0] == 'V' || label[0] == '?') &&
		(label[1] == '[' || label[1] == '#');
	if (tipStart != std::string::npos || arrowTip || (!label.empty() && label[0] == '#')) {
		entry.toggleable = false;
		entry.sourceIndex = -1;
		entry.kind = CheatMenuEntryKind::Tip;
		size_t textStart = 0;
		if (tipStart != std::string::npos) {
			textStart = tipStart + 2;
		} else if (label[0] == '#') {
			textStart = 1;
		} else {
			textStart = label[1] == '#' ? 2 : 1;
		}
		label = label.substr(textStart);
		const size_t close = label.find(']');
		if (close != std::string::npos) {
			label.resize(close);
		}
		TrimCheatDecorators(&label);
		if (!label.empty()) {
			entry.name = label;
		}
		return entry;
	}

	const size_t arrowOpen = label.find(">>>");
	const size_t arrowClose = label.find("<<<");
	const int underscoreCount = (int)std::count(label.begin(), label.end(), '_');
	const size_t bracketOpen = label.find('[');
	const size_t bracketClose = label.rfind(']');
	if (arrowOpen != std::string::npos || arrowClose != std::string::npos ||
		(underscoreCount >= 6 && bracketOpen != std::string::npos && bracketClose != std::string::npos && bracketClose > bracketOpen)) {
		entry.toggleable = false;
		entry.sourceIndex = -1;
		entry.kind = CheatMenuEntryKind::Section;
		if (arrowOpen != std::string::npos) {
			const size_t textStart = arrowOpen + 3;
			const size_t textEnd = arrowClose != std::string::npos && arrowClose > textStart ? arrowClose : label.size();
			label = label.substr(textStart, textEnd - textStart);
		} else {
			label = label.substr(bracketOpen + 1, bracketClose - bracketOpen - 1);
		}
		TrimCheatDecorators(&label);
		if (!label.empty()) {
			entry.name = label;
		}
	}

	return entry;
}

std::vector<CheatMenuEntry> LoadCheatEntries() {
	std::vector<CheatMenuEntry> entries;
	const std::string gameID = g_paramSFO.GetDiscID();
	if (gameID.length() != 9) {
		return entries;
	}

	CWCheatEngine engine(gameID);
	engine.CreateCheatFile();
	const std::vector<CheatFileInfo> fileInfo = engine.FileInfo();
	entries.reserve(fileInfo.size());
	for (int i = 0; i < (int)fileInfo.size(); ++i) {
		const CheatFileInfo &info = fileInfo[i];
		if (info.name.empty()) {
			continue;
		}
		CheatMenuEntry entry = MakeCheatMenuEntry(info, i);
		if (!entry.toggleable) {
			for (const std::string &line : SplitCheatMetadataLines(entry.name)) {
				CheatMenuEntry lineEntry = entry;
				lineEntry.name = line;
				entries.push_back(std::move(lineEntry));
			}
		} else {
			entries.push_back(std::move(entry));
		}
	}
	return entries;
}

struct CheatInfoResult {
	bool enabled = false;
	bool available = false;
	std::vector<CheatMenuEntry> entries;
};

CheatInfoResult BuildCheatInfo(bool importFromDb) {
	CheatInfoResult result;
	if (!g_Config.bEnableCheats) {
		return result;
	}

	const Path cheatDbPath = GetCheatDbPath();
	if (importFromDb && !cheatDbPath.empty()) {
		ImportCheatsFromDb(cheatDbPath);
	}

	result.enabled = true;
	result.entries = LoadCheatEntries();
	result.available = !cheatDbPath.empty() || !result.entries.empty();
	return result;
}

void RefreshCheatInfo(bool importFromDb = false) {
	CheatInfoResult result = BuildCheatInfo(importFromDb);
	g_state.overlay.SetCheatInfo(result.enabled, result.available, result.entries);
}

void CloseCheatLoadThreadHandle() {
	if (g_state.cheatLoadThreadCreated) {
		threadWaitForExit(&g_state.cheatLoadThread);
		threadClose(&g_state.cheatLoadThread);
		g_state.cheatLoadThread = {};
		g_state.cheatLoadThreadCreated = false;
	}
	g_state.cheatLoadThreadActive.store(false, std::memory_order_release);
}

void WaitForCheatLoadThread() {
	if (!g_state.cheatLoadThreadCreated && !g_state.cheatLoadThreadActive.load(std::memory_order_acquire)) {
		return;
	}
	CloseCheatLoadThreadHandle();
}

void CheatLoadThreadEntry(void *) {
	SetCurrentThreadName("TicoCheats");
	std::shared_ptr<CheatInfoResult> result = std::make_shared<CheatInfoResult>(BuildCheatInfo(true));
	QueueMainThread([result]() {
		CloseCheatLoadThreadHandle();
		g_state.overlay.SetCheatInfo(result->enabled, result->available, result->entries);
	});
}

void StartAsyncCheatLoad() {
	bool expected = false;
	if (!g_state.cheatLoadThreadActive.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		return;
	}

	Result rc = threadCreate(&g_state.cheatLoadThread, CheatLoadThreadEntry, nullptr, nullptr,
		kCheatLoadThreadStackSize, kCheatLoadThreadPriority, kCheatLoadThreadCore);
	if (R_FAILED(rc)) {
		Log("cheat load threadCreate failed rc=0x%x", (unsigned)rc);
		g_state.cheatLoadThreadActive.store(false, std::memory_order_release);
		RefreshCheatInfo(true);
		return;
	}
	g_state.cheatLoadThreadCreated = true;

	rc = threadStart(&g_state.cheatLoadThread);
	if (R_FAILED(rc)) {
		Log("cheat load threadStart failed rc=0x%x", (unsigned)rc);
		threadClose(&g_state.cheatLoadThread);
		g_state.cheatLoadThread = {};
		g_state.cheatLoadThreadCreated = false;
		g_state.cheatLoadThreadActive.store(false, std::memory_order_release);
		RefreshCheatInfo(true);
	}
}

void RefreshCheatAvailability() {
	g_state.overlay.SetCheatsEnabled(g_Config.bEnableCheats);
}

bool ToggleCheatLine(int index) {
	const std::string gameID = g_paramSFO.GetDiscID();
	if (gameID.length() != 9) {
		Log("tico cheat toggle skipped invalid game id=%s", gameID.c_str());
		return false;
	}

	CWCheatEngine engine(gameID);
	engine.CreateCheatFile();
	std::vector<CheatFileInfo> fileInfo = engine.FileInfo();
	if (index < 0 || index >= (int)fileInfo.size()) {
		Log("tico cheat toggle skipped invalid index=%d count=%u", index, (unsigned)fileInfo.size());
		return false;
	}

	CheatFileInfo target = fileInfo[index];
	target.enabled = !target.enabled;

	FILE *in = File::OpenCFile(engine.CheatFilename(), "rt");
	if (!in) {
		Log("tico cheat toggle open failed path=%s", engine.CheatFilename().ToString().c_str());
		return false;
	}

	std::vector<std::string> lines;
	while (!feof(in)) {
		char temp[2048]{};
		char *line = ReadCheatDbLine(temp, sizeof(temp), in);
		if (!line) {
			break;
		}
		lines.push_back(line);
	}
	fclose(in);

	const size_t lineIndex = target.lineNum > 0 ? (size_t)target.lineNum - 1 : (size_t)-1;
	if (lineIndex >= lines.size()) {
		Log("tico cheat toggle line out of range index=%d line=%d count=%u", index, target.lineNum, (unsigned)lines.size());
		return false;
	}

	std::string &line = lines[lineIndex];
	if (line.find("_C") == std::string::npos || line.find(target.name) == std::string::npos) {
		Log("tico cheat toggle line mismatch index=%d line=%d name=%s", index, target.lineNum, target.name.c_str());
		return false;
	}
	line = (target.enabled ? "_C1 " : "_C0 ") + target.name;

	FILE *out = File::OpenCFile(engine.CheatFilename(), "wt");
	if (!out) {
		Log("tico cheat toggle write failed path=%s", engine.CheatFilename().ToString().c_str());
		return false;
	}

	for (size_t i = 0; i < lines.size(); ++i) {
		fprintf(out, "%s", lines[i].c_str());
		if (i + 1 < lines.size()) {
			fputc('\n', out);
		}
	}
	fclose(out);

	g_Config.bReloadCheats = true;
	Log("tico cheat toggled index=%d enabled=%d name=%s", index, target.enabled ? 1 : 0, target.name.c_str());
	return true;
}

void ToggleCheatFromQuickMenu(int index) {
	if (!g_Config.bEnableCheats) {
		Log("tico cheat toggle skipped: cheats disabled in core config");
		RefreshCheatInfo(false);
		return;
	}
	if (ToggleCheatLine(index)) {
		RefreshCheatInfo(false);
	}
}

std::string GetLegacySaveStateBaseName() {
	std::string romName = g_state.contentPath;

	size_t lastSlash = romName.find_last_of("/\\");
	if (lastSlash != std::string::npos) {
		romName = romName.substr(lastSlash + 1);
	}

	size_t lastDot = romName.find_last_of('.');
	if (lastDot != std::string::npos) {
		romName = romName.substr(0, lastDot);
	}

	return romName;
}

Path GetLegacySaveStatePath(int slot) {
	const std::string romName = GetLegacySaveStateBaseName();
	if (romName.empty()) {
		return Path();
	}

	const int safeSlot = std::clamp(slot, 0, Ppsspp::SaveStateSlotCount - 1);
	return Path(Paths::PpssppSaveStates) / (romName + ".state" + std::to_string(safeSlot));
}

u64 GetSystemMs() {
	const u64 tickFreq = armGetSystemTickFreq();
	const u64 ticksPerMs = tickFreq / 1000;
	return ticksPerMs > 0 ? armGetSystemTick() / ticksPerMs : 0;
}

void RefreshSaveStateSlots(bool force) {
	const u64 nowMs = GetSystemMs();
	if (!force && nowMs != 0 && nowMs - g_state.lastSaveStateScanMs < 500) {
		return;
	}

	File::CreateFullPath(Path(Paths::PpssppSaveStates));
	for (int i = 0; i < Ppsspp::SaveStateSlotCount; ++i) {
		const Path statePath = GetLegacySaveStatePath(i);
		g_state.saveStateSlots[i] = !statePath.empty() && File::Exists(statePath);
	}
	g_state.overlay.SetSaveStateInfo(g_Config.iCurrentStateSlot, g_state.saveStateSlots);
	g_state.lastSaveStateScanMs = nowMs;
}

void AfterSaveStateAction(SaveState::Status status, std::string_view message) {
	Log("tico savestate status=%d message=%.*s", (int)status, (int)message.size(), message.data());
	RefreshSaveStateSlots(true);
}

bool WaitForPspBoot(const char *tag, std::string *errorString) {
	int bootLoopCount = 0;
	while (g_state.running && appletMainLoop() && PSP_InitUpdate(errorString) == BootState::Booting) {
		if (bootLoopCount == 0) {
			Log("%s loop entered", tag);
		}
		bootLoopCount++;
		DrainMainThreadQueue();
		RetroAchievements().Idle();
		if (g_state.graphicsContext) {
			g_state.graphicsContext->Poll();
		}
		sleep_ms(1, "tico-boot-poll");
	}
	Log("%s loop exited count=%d state=%d", tag, bootLoopCount, (int)PSP_GetBootState());
	return PSP_IsInited();
}

void ResetContent() {
	if (!PSP_IsInited()) {
		Log("tico reset ignored: PSP is not initialized");
		return;
	}

	const std::string path = g_state.contentPath;
	Log("tico reset content path=%s", path.c_str());
	RetroAchievements().UnloadGame();
	PSP_Shutdown(true);
	g_state.ppssppShutdown = false;
	g_state.booted = false;
	Core_SetGraphicsContext(g_state.graphicsContext);
	PSP_CoreParameter().graphicsContext = g_state.graphicsContext;

	if (!PSP_InitStart(PSP_CoreParameter())) {
		Log("tico reset PSP_InitStart failed");
		g_state.running = false;
		Core_Stop();
		return;
	}

	std::string errorString;
	if (!WaitForPspBoot("reset", &errorString)) {
		Log("tico reset failed: %s", errorString.c_str());
		g_state.running = false;
		Core_Stop();
		return;
	}

	System_Notify(SystemNotification::BOOT_DONE);
	coreState = CORE_RUNNING_CPU;
	g_state.booted = true;
	RetroAchievements().SetGame(path);
	RefreshSaveStateSlots(true);
	Log("tico reset complete");
}

void ExecuteOverlayCommand(OverlayCommand command) {
	if (command.action == OverlayAction::None) {
		return;
	}

	if (command.action == OverlayAction::Reset) {
		ResetContent();
		return;
	}
	if (command.action == OverlayAction::LoadCheats) {
		StartAsyncCheatLoad();
		return;
	}
	if (command.action == OverlayAction::ToggleCheat) {
		ToggleCheatFromQuickMenu(command.slot);
		return;
	}

	const int slot = std::clamp(command.slot, 0, Ppsspp::SaveStateSlotCount - 1);
	const Path statePath = GetLegacySaveStatePath(slot);
	const std::string statePathString = statePath.ToString();
	g_Config.iCurrentStateSlot = slot;
	if (RetroAchievements().WarnIfHardcoreModeActive(command.action == OverlayAction::SaveState || command.action == OverlayAction::LoadState)) {
		return;
	}
	if (statePath.empty()) {
		Log("tico savestate ignored: empty content path slot=%d", slot);
		return;
	}

	File::CreateFullPath(Path(Paths::PpssppSaveStates));

	if (command.action == OverlayAction::SaveState) {
		Log("tico save state slot=%d path=%s", slot, statePathString.c_str());
		SaveState::Save(statePath, slot, &AfterSaveStateAction);
	} else if (command.action == OverlayAction::LoadState) {
		if (!File::Exists(statePath)) {
			Log("tico load state missing slot=%d path=%s", slot, statePathString.c_str());
			RefreshSaveStateSlots(true);
			return;
		}
		Log("tico load state slot=%d path=%s", slot, statePathString.c_str());
		SaveState::Load(statePath, slot, &AfterSaveStateAction);
	}
}

}  // namespace

PpssppRuntime::PpssppRuntime(LogCallback log) : log_(std::move(log)) {
	g_state.log = log_;
}

PpssppRuntime::~PpssppRuntime() = default;

bool PpssppRuntime::Configure(const LaunchInfo &launch) {
	g_state.running = true;
	g_state.booted = false;
	g_state.ppssppShutdown = false;
	g_state.chainloadLauncher = false;
	g_state.frameOpen = false;
	g_state.hostFrameOpen = false;
	g_state.contentPath = launch.contentPath;
	g_state.log = log_;
	PROFILE_INIT();
	TimeInit();
	Log("ppsspp runtime configure content=%s", g_state.contentPath.c_str());
	return !g_state.contentPath.empty();
}

bool PpssppRuntime::Initialize(const LaunchInfo &) {
	UpdateDisplayMode();
	Log("initial display=%dx%d", g_display.pixel_xres, g_display.pixel_yres);

	InitAudio();
	Log("InitAudio ready=%d rate=%d samples=%d", g_state.audioReady ? 1 : 0, g_state.audioSampleRate, g_state.audioBufferSamples);
	if (!InstallPpssppAssets(g_state.log)) {
		Log("InstallPpssppAssets failed");
		return false;
	}
	TrophySfx().Load(g_state.log);
	InitializeConfig();
	Log("InitializeConfig backend=%d mt=%d inflight=%d", g_Config.iGPUBackend, g_Config.bRenderMultiThreading ? 1 : 0, g_Config.iInflightFrames);

	g_logManager.Init(&g_Config.bEnableLogging, false);
	if (g_Config.bEnableLogging && g_Config.bEnableFileLogging) {
		g_logManager.SetAllLogLevels(LogLevel::LINFO);
		g_logManager.EnableOutput(LogOutput::File);
		g_logManager.SetFileLogPath(::Path(Paths::PpssppDataRoot) / "SYSTEM/DUMP/log.txt");
		Log("ppsspp log=%s", g_logManager.GetLogFilePath().c_str());
	} else {
		Log("ppsspp file log disabled");
	}
	g_threadManager.Init(cpu_info.num_cores, cpu_info.logical_cpu_count);
	Log("thread manager cores=%d logical=%d", cpu_info.num_cores, cpu_info.logical_cpu_count);
	RetroAchievements().Initialize(g_state.log);

	std::string errorString;
	const GPUCore gpuCore = GPUCORE_VULKAN;
	Log("graphics init start gpuCore=%d", (int)gpuCore);
	if (!g_state.graphicsHost.Init(&errorString, &g_state.graphicsContext, gpuCore)) {
		std::fprintf(stderr, "Graphics initialization failed: %s\n", errorString.c_str());
		Log("graphics init failed: %s", errorString.c_str());
		return false;
	}
	g_state.draw = g_state.graphicsContext ? g_state.graphicsContext->GetDrawContext() : nullptr;
	Log("graphics init ok ctx=%p draw=%p", g_state.graphicsContext, g_state.draw);
	return true;
}

bool PpssppRuntime::LoadContent(const std::string &path) {
	CoreParameter coreParameter{};
	coreParameter.cpuCore = CPUCore::JIT;
	coreParameter.gpuCore = GPUCORE_VULKAN;
	coreParameter.graphicsContext = g_state.graphicsContext;
	coreParameter.enableSound = g_state.audioReady;
	coreParameter.fileToStart = ::Path(path);
	coreParameter.headLess = false;
	coreParameter.renderScaleFactor = 1;
	coreParameter.renderWidth = 480;
	coreParameter.renderHeight = 272;
	coreParameter.pixelWidth = g_display.pixel_xres;
	coreParameter.pixelHeight = g_display.pixel_yres;
	coreParameter.fastForward = false;

	Core_SetGraphicsContext(g_state.graphicsContext);
	Log("PSP_InitStart file=%s", path.c_str());
	if (!PSP_InitStart(coreParameter)) {
		std::fprintf(stderr, "Failed to start PSP core for '%s'\n", path.c_str());
		Log("PSP_InitStart failed");
		return false;
	}

	std::string errorString;
	if (!WaitForPspBoot("boot", &errorString)) {
		std::fprintf(stderr, "Startup failed: %s\n", errorString.c_str());
		Log("startup failed: %s", errorString.c_str());
		return false;
	}

	System_Notify(SystemNotification::BOOT_DONE);
	coreState = CORE_RUNNING_CPU;
	g_state.booted = true;
	RetroAchievements().SetGame(path);
	Log("boot complete");
	return true;
}

void PpssppRuntime::HandleInput(const FrameInput &input) {
	const bool overlayToggleHeld = (input.buttons & HidNpadButton_Plus) && (input.buttons & HidNpadButton_Minus);
	const bool overlayTogglePressed = overlayToggleHeld && ((input.pressed & HidNpadButton_Plus) || (input.pressed & HidNpadButton_Minus));
	if (g_state.overlay.IsVisible() || overlayTogglePressed) {
		RefreshSaveStateSlots(overlayTogglePressed);
		if (overlayTogglePressed) {
			RefreshCheatAvailability();
		}
	}
	const bool inputConsumedByOverlay = g_state.overlay.HandleInput(input.buttons, input.pressed,
		input.leftStickX, input.leftStickY, input.rightStickX, input.rightStickY);
	ExecuteOverlayCommand(g_state.overlay.ConsumeCommand());
	if (g_state.overlay.ShouldExitGame()) {
		Log("tico overlay exit requested");
		g_state.overlay.ClearExitRequest();
		g_state.chainloadLauncher = true;
		RequestExit();
		return;
	}

	if (inputConsumedByOverlay) {
		ClearPspInput();
	} else {
		UpdatePspInput(input);
	}
}

void PpssppRuntime::RunFrame() {
	if (!g_state.running || !g_state.booted) {
		return;
	}

	if (g_state.frameCount == 0) {
		Log("main loop entered");
	}
	g_state.frameCount++;
	UpdateDisplayMode();
	if (g_state.graphicsContext) {
		g_state.graphicsContext->Poll();
	}
	DrainMainThreadQueue();
	RetroAchievements().Idle();
	PSP_UpdateDebugStats((DebugOverlay)g_Config.iDebugOverlay == DebugOverlay::DEBUG_STATS || g_Config.bLogFrameDrops);

	const DisplayLayoutConfig &displayLayoutConfig = g_Config.GetDisplayLayoutConfig(DeviceOrientation::Landscape);
	__DisplaySetDisplayLayoutConfig(displayLayoutConfig);

	// Save/load requests are queued by SaveState::Save/Load and must be
	// flushed explicitly each frame, matching PPSSPP's normal EmuScreen path.
	SaveState::Process();

	if (g_state.draw) {
		g_state.draw->BeginFrame(Draw::DebugFlags::NONE);
		g_state.frameOpen = true;
		if (!g_state.overlay.IsReady()) {
			g_state.overlay.Init(g_state.draw, g_state.contentPath.c_str(), g_state.log);
			RefreshCheatAvailability();
		}
	}
	if (gpu) {
		gpu->BeginHostFrame(displayLayoutConfig);
		g_state.hostFrameOpen = true;
	}

	if (g_state.overlay.IsVisible()) {
		sleep_ms(1, "tico-overlay-pause");
	} else {
		RetroAchievements().FrameUpdate();
		PSP_RunLoopWhileState();
	}

	if (coreState == CORE_NEXTFRAME) {
		coreState = CORE_RUNNING_CPU;
	}
	if (gpu) {
		gpu->EndHostFrame();
		g_state.hostFrameOpen = false;
		if (!gpu->PresentedThisFrame()) {
			gpu->PrepareCopyDisplayToOutput(displayLayoutConfig);
		}
	}
	if (coreState == CORE_POWERDOWN) {
		RequestExit();
	}
}

void PpssppRuntime::RenderFrame() {
	if (!g_state.frameOpen || !g_state.draw) {
		return;
	}

	const DisplayLayoutConfig &displayLayoutConfig = g_Config.GetDisplayLayoutConfig(DeviceOrientation::Landscape);
	if (gpu && !gpu->PresentedThisFrame()) {
		g_state.draw->BindFramebufferAsRenderTarget(nullptr, { Draw::RPAction::CLEAR, Draw::RPAction::CLEAR, Draw::RPAction::CLEAR }, "TicoPpsspp");
		gpu->CopyDisplayToOutput(displayLayoutConfig);
	}
	g_state.overlay.Render(g_state.draw);
	g_state.draw->EndFrame();
	g_state.frameOpen = false;
	g_frameTiming.PostSubmit();
	g_state.draw->Present(Draw::PresentMode::FIFO);
}

bool PpssppRuntime::ShouldExit() const {
	return !g_state.running || coreState == CORE_POWERDOWN;
}

bool PpssppRuntime::ShouldChainloadLauncher() const {
	return g_state.chainloadLauncher;
}

void PpssppRuntime::RequestExit() {
	g_state.running = false;
	Core_Stop();
}

void PpssppRuntime::Shutdown() {
	Log("ppsspp runtime shutdown start frames=%d coreState=%d running=%d", g_state.frameCount, (int)coreState, g_state.running ? 1 : 0);
	WaitForCheatLoadThread();
	DrainMainThreadQueue();
	if (g_state.hostFrameOpen && gpu) {
		gpu->EndHostFrame();
		g_state.hostFrameOpen = false;
	}
	if (g_state.frameOpen && g_state.draw) {
		g_state.draw->EndFrame();
		g_state.frameOpen = false;
	}
	RetroAchievements().Shutdown();
	if (PSP_IsInited() && !g_state.ppssppShutdown) {
		PSP_Shutdown(true);
		g_state.ppssppShutdown = true;
		Log("PSP_Shutdown complete");
	}
	g_state.overlay.Shutdown();
	TrophySfx().Shutdown();
	ShutdownAudio();
	g_state.graphicsHost.Shutdown();
	g_state.graphicsContext = nullptr;
	g_state.draw = nullptr;
	g_VFS.Clear();
	g_logManager.Shutdown();
	g_threadManager.Teardown();
}

int RuntimeAudioSampleRate() {
	return g_state.audioSampleRate > 0 ? g_state.audioSampleRate : kAudioSampleRate;
}

int RuntimeAudioBufferSamples() {
	return g_state.audioBufferSamples > 0 ? g_state.audioBufferSamples : kAudioSamples;
}

void RuntimeRequestExit() {
	g_state.running = false;
	Core_Stop();
}

void RuntimeRunOnMainThread(std::function<void()> func) {
	QueueMainThread(std::move(func));
}

void RuntimeAudioGetDebugStats(char *buf, size_t bufSize) {
	if (buf) {
		g_state.resampler.GetAudioDebugStats(buf, bufSize);
	} else {
		g_state.resampler.ResetStatCounters();
	}
}

void RuntimeAudioClear() {
	g_state.resampler.Clear();
}

void RuntimeAudioPushSamples(const s32 *audio, int numSamples, float volume) {
	if (audio && g_state.audioReady) {
		g_state.resampler.PushSamples(audio, numSamples, volume);
	} else {
		g_state.resampler.Clear();
	}
}

}  // namespace Tico

void NativeFrame(GraphicsContext *) {
}

void NativeResized() {
}

void System_Toast(std::string_view text) {
	std::fprintf(stderr, "%.*s\n", (int)text.size(), text.data());
}

void System_ShowKeyboard() {
}

void System_Vibrate(int) {
}

void System_LaunchUrl(LaunchUrlType, std::string_view) {
}

std::string System_GetProperty(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_NAME:
		return "PPSSPP Tico";
	case SYSPROP_AUDIO_DEVICE_LIST:
		return "";
	default:
		return "";
	}
}

std::vector<std::string> System_GetPropertyStringVec(SystemProperty) {
	return {};
}

int64_t System_GetPropertyInt(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_SYSTEMVERSION:
		return 31;
	case SYSPROP_DISPLAY_XRES:
		return g_display.pixel_xres;
	case SYSPROP_DISPLAY_YRES:
		return g_display.pixel_yres;
	case SYSPROP_DISPLAY_COUNT:
		return 1;
	case SYSPROP_DISPLAY_DPI:
	case SYSPROP_DISPLAY_LOGICAL_DPI:
		return 160;
	case SYSPROP_AUDIO_SAMPLE_RATE:
	case SYSPROP_AUDIO_OPTIMAL_SAMPLE_RATE:
		return Tico::RuntimeAudioSampleRate();
	case SYSPROP_AUDIO_FRAMES_PER_BUFFER:
	case SYSPROP_AUDIO_OPTIMAL_FRAMES_PER_BUFFER:
		return Tico::RuntimeAudioBufferSamples();
	case SYSPROP_DEVICE_TYPE:
		return DEVICE_TYPE_DESKTOP;
	default:
		return -1;
	}
}

float System_GetPropertyFloat(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_DISPLAY_REFRESH_RATE:
		return 60.0f;
	case SYSPROP_DISPLAY_DPI:
	case SYSPROP_DISPLAY_LOGICAL_DPI:
		return 160.0f;
	case SYSPROP_DISPLAY_SAFE_INSET_LEFT:
	case SYSPROP_DISPLAY_SAFE_INSET_RIGHT:
	case SYSPROP_DISPLAY_SAFE_INSET_TOP:
	case SYSPROP_DISPLAY_SAFE_INSET_BOTTOM:
		return 0.0f;
	default:
		return -1.0f;
	}
}

bool System_GetPropertyBool(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_CAN_JIT:
		return true;
	case SYSPROP_SKIP_UI:
		return false;
	case SYSPROP_SUPPORTS_HTTPS:
		return false;
	case SYSPROP_SUPPORTS_PERMISSIONS:
	case SYSPROP_HAS_BACK_BUTTON:
	case SYSPROP_HAS_KEYBOARD:
	case SYSPROP_HAS_ACCELEROMETER:
	case SYSPROP_APP_GOLD:
		return false;
	default:
		return false;
	}
}

void System_Notify(SystemNotification) {
}

void System_PostUIMessage(UIMessage, std::string_view) {
}

void System_RunOnMainThread(std::function<void()> func) {
	Tico::RuntimeRunOnMainThread(std::move(func));
}

bool System_MakeRequest(SystemRequestType type, int, const std::string &param1, const std::string &, int64_t, int64_t) {
	switch (type) {
	case SystemRequestType::SEND_DEBUG_OUTPUT:
		if (!param1.empty()) {
			fwrite(param1.data(), sizeof(char), param1.size(), stdout);
		}
		return true;
	case SystemRequestType::EXIT_APP:
	case SystemRequestType::RESTART_APP:
		Tico::RuntimeRequestExit();
		return true;
	default:
		return false;
	}
}

void System_AskForPermission(SystemPermission) {
}

PermissionStatus System_GetPermissionStatus(SystemPermission) {
	return PERMISSION_STATUS_GRANTED;
}

void System_AudioGetDebugStats(char *buf, size_t bufSize) {
	Tico::RuntimeAudioGetDebugStats(buf, bufSize);
}

void System_AudioClear() {
	Tico::RuntimeAudioClear();
}

void System_AudioPushSamples(const s32 *audio, int numSamples, float volume) {
	Tico::RuntimeAudioPushSamples(audio, numSamples, volume);
}

bool NativeSaveSecret(std::string_view, std::string_view) {
	return false;
}

std::string NativeLoadSecret(std::string_view) {
	return "";
}

#endif
