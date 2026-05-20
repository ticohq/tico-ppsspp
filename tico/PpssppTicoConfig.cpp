#include "PpssppTicoConfig.h"

#include "Common/File/Path.h"
#include "Common/System/Display.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/HLE/sceUtility.h"
#include "GPU/Common/TextureScalerCommon.h"

#include <algorithm>
#include <utility>

namespace Tico {
namespace {

constexpr float kPspNativeWidth = 480.0f;
constexpr float kPspNativeHeight = 272.0f;

constexpr const char *kDefaultPpssppCoreConfig = R"json({
    "ppsspp_cpu_core": "JIT",
    "ppsspp_fast_memory": "enabled",
    "ppsspp_ignore_bad_memory_access": "enabled",
    "ppsspp_io_timing_method": "Fast",
    "ppsspp_force_lag_sync": "disabled",
    "ppsspp_locked_cpu_speed": "0",
    "ppsspp_cache_iso": "disabled",
    "ppsspp_cheats": "disabled",
    "ppsspp_psp_model": "psp_2000_3000",
    "ppsspp_button_preference": "Cross",
    "ppsspp_internal_resolution": "480x272",
    "ppsspp_software_rendering": "disabled",
    "ppsspp_rendering_mode": "buffered",
    "ppsspp_gpu_hardware_transform": "enabled",
    "ppsspp_texture_filtering": "Auto",
    "ppsspp_texture_anisotropic_filtering": "Off",
    "ppsspp_lower_resolution_for_effects": "Off",
    "ppsspp_texture_deposterize": "disabled",
    "ppsspp_texture_scaling_type": "xbrz",
    "ppsspp_texture_scaling_level": "1",
    "ppsspp_texture_replacement": "enabled",
    "ppsspp_skip_buffer_effects": "disabled",
    "ppsspp_frameskip": "0",
    "ppsspp_auto_frameskip": "disabled",
    "ppsspp_frame_duplication": "disabled",
    "ppsspp_detect_vsync_swap_interval": "disabled",
    "ppsspp_inflight_frames": "Up to 2",
    "ppsspp_analog_is_circular": "disabled",
    "ppsspp_analog_deadzone": "0.15",
    "ppsspp_analog_sensitivity": "1.10",
    "ppsspp_language": "Automatic",
    "ppsspp_memstick_inserted": "enabled",
    "ppsspp_cropto16x9": "disabled",
    "ppsspp_block_transfer_gpu": "enabled",
    "ppsspp_disable_range_culling": "disabled",
    "display_mode": "Display",
    "display_size": "16:9",
    "integer_scale": "Auto"
})json";

void ApplyPpssppOptions(const std::map<std::string, std::string> &options) {
	auto applyBool = [&](const char *key, bool &setting) {
		if (const std::string *value = FindOption(options, key)) {
			setting = OptionEnabled(*value);
		}
	};

	if (const std::string *value = FindOption(options, "ppsspp_cpu_core")) {
		if (*value == "JIT") {
			g_Config.iCpuCore = (int)CPUCore::JIT;
		} else if (*value == "IR JIT") {
			g_Config.iCpuCore = (int)CPUCore::IR_INTERPRETER;
		} else if (*value == "Interpreter") {
			g_Config.iCpuCore = (int)CPUCore::INTERPRETER;
		}
	}
	applyBool("ppsspp_fast_memory", g_Config.bFastMemory);
	applyBool("ppsspp_ignore_bad_memory_access", g_Config.bIgnoreBadMemAccess);
	applyBool("ppsspp_force_lag_sync", g_Config.bForceLagSync);
	applyBool("ppsspp_cache_iso", g_Config.bCacheFullIsoInRam);
	applyBool("ppsspp_cheats", g_Config.bEnableCheats);
	applyBool("ppsspp_analog_is_circular", g_Config.bAnalogIsCircular);
	applyBool("ppsspp_memstick_inserted", g_Config.bMemStickInserted);
	applyBool("ppsspp_software_rendering", g_Config.bSoftwareRendering);
	applyBool("ppsspp_cropto16x9", g_Config.bDisplayCropTo16x9);
	applyBool("ppsspp_auto_frameskip", g_Config.bAutoFrameSkip);
	applyBool("ppsspp_frame_duplication", g_Config.bRenderDuplicateFrames);
	applyBool("ppsspp_skip_buffer_effects", g_Config.bSkipBufferEffects);
	applyBool("ppsspp_disable_range_culling", g_Config.bDisableRangeCulling);
	applyBool("ppsspp_gpu_hardware_transform", g_Config.bHardwareTransform);
	applyBool("ppsspp_software_skinning", g_Config.bSoftwareSkinning);
	applyBool("ppsspp_hardware_tesselation", g_Config.bHardwareTessellation);
	applyBool("ppsspp_texture_deposterize", g_Config.bTexDeposterize);
	applyBool("ppsspp_texture_replacement", g_Config.bReplaceTextures);
	applyBool("ppsspp_smart_2d_texture_filtering", g_Config.bSmart2DTexFiltering);
	applyBool("ppsspp_lazy_texture_caching", g_Config.bTextureBackoffCache);
	if (const std::string *value = FindOption(options, "ppsspp_io_timing_method")) {
		if (*value == "Fast") {
			g_Config.iIOTimingMethod = IOTIMING_FAST;
		} else if (*value == "Host") {
			g_Config.iIOTimingMethod = IOTIMING_HOST;
		} else if (*value == "Simulate UMD delays") {
			g_Config.iIOTimingMethod = IOTIMING_REALISTIC;
		} else if (*value == "Simulate UMD slow reading speed") {
			g_Config.iIOTimingMethod = IOTIMING_UMDSLOWREALISTIC;
		}
	}
	if (const std::string *value = FindOption(options, "ppsspp_locked_cpu_speed")) {
		g_Config.iLockedCPUSpeed = OptionInt(*value, g_Config.iLockedCPUSpeed);
	}
	if (const std::string *value = FindOption(options, "ppsspp_psp_model")) {
		if (*value == "psp_1000") {
			g_Config.iPSPModel = PSP_MODEL_FAT;
		} else if (*value == "psp_2000_3000") {
			g_Config.iPSPModel = PSP_MODEL_SLIM;
		}
	}
	if (const std::string *value = FindOption(options, "ppsspp_button_preference")) {
		if (*value == "Cross") {
			g_Config.iButtonPreference = PSP_SYSTEMPARAM_BUTTON_CROSS;
		} else if (*value == "Circle") {
			g_Config.iButtonPreference = PSP_SYSTEMPARAM_BUTTON_CIRCLE;
		}
	}
	if (const std::string *value = FindOption(options, "ppsspp_analog_deadzone")) {
		g_Config.fAnalogDeadzone = OptionFloat(*value, g_Config.fAnalogDeadzone);
	}
	if (const std::string *value = FindOption(options, "ppsspp_analog_sensitivity")) {
		g_Config.fAnalogSensitivity = OptionFloat(*value, g_Config.fAnalogSensitivity);
	}
	if (const std::string *value = FindOption(options, "ppsspp_internal_resolution")) {
		if (*value == "480x272") g_Config.iInternalResolution = 1;
		else if (*value == "960x544") g_Config.iInternalResolution = 2;
		else if (*value == "1440x816") g_Config.iInternalResolution = 3;
		else if (*value == "1920x1088") g_Config.iInternalResolution = 4;
		else if (*value == "2400x1360") g_Config.iInternalResolution = 5;
		else if (*value == "2880x1632") g_Config.iInternalResolution = 6;
		else if (*value == "3360x1904") g_Config.iInternalResolution = 7;
		else if (*value == "3840x2176") g_Config.iInternalResolution = 8;
		else if (*value == "4320x2448") g_Config.iInternalResolution = 9;
		else if (*value == "4800x2720") g_Config.iInternalResolution = 10;
		else g_Config.iInternalResolution = OptionInt(*value, g_Config.iInternalResolution);
	}
	if (const std::string *value = FindOption(options, "ppsspp_frameskip")) {
		g_Config.iFrameSkip = OptionInt(*value, g_Config.iFrameSkip);
	}
	const std::string *multiSampleValue = FindOption(options, "ppsspp_multisample_level");
	if (!multiSampleValue) {
		multiSampleValue = FindOption(options, "ppsspp_mulitsample_level");
	}
	if (multiSampleValue) {
		if (*multiSampleValue == "Disabled") g_Config.iMultiSampleLevel = 0;
		else if (*multiSampleValue == "x2") g_Config.iMultiSampleLevel = 1;
		else if (*multiSampleValue == "x4") g_Config.iMultiSampleLevel = 2;
		else if (*multiSampleValue == "x8") g_Config.iMultiSampleLevel = 3;
	}
	if (const std::string *value = FindOption(options, "ppsspp_inflight_frames")) {
		if (*value == "No buffer") g_Config.iInflightFrames = 1;
		else if (*value == "Up to 1") g_Config.iInflightFrames = 2;
		else if (*value == "Up to 2") g_Config.iInflightFrames = 3;
		else g_Config.iInflightFrames = std::clamp(OptionInt(*value, g_Config.iInflightFrames), 1, 3);
	}
	if (const std::string *value = FindOption(options, "ppsspp_lower_resolution_for_effects")) {
		if (*value == "Off" || *value == "disabled") g_Config.iBloomHack = 0;
		else if (*value == "Safe") g_Config.iBloomHack = 1;
		else if (*value == "Balanced") g_Config.iBloomHack = 2;
		else if (*value == "Aggressive") g_Config.iBloomHack = 3;
	}
	if (const std::string *value = FindOption(options, "ppsspp_skip_gpu_readbacks")) {
		g_Config.iSkipGPUReadbackMode = OptionEnabled(*value) ? (int)SkipGPUReadbackMode::SKIP : (int)SkipGPUReadbackMode::NO_SKIP;
	}
	if (const std::string *value = FindOption(options, "ppsspp_spline_quality")) {
		if (*value == "Low") g_Config.iSplineBezierQuality = 0;
		else if (*value == "Medium") g_Config.iSplineBezierQuality = 1;
		else if (*value == "High") g_Config.iSplineBezierQuality = 2;
	}
	if (const std::string *value = FindOption(options, "ppsspp_texture_scaling_type")) {
		if (*value == "xbrz") g_Config.iTexScalingType = TextureScalerCommon::XBRZ;
		else if (*value == "hybrid") g_Config.iTexScalingType = TextureScalerCommon::HYBRID;
		else if (*value == "bicubic") g_Config.iTexScalingType = TextureScalerCommon::BICUBIC;
		else if (*value == "hybrid_bicubic") g_Config.iTexScalingType = TextureScalerCommon::HYBRID_BICUBIC;
	}
	if (const std::string *value = FindOption(options, "ppsspp_texture_scaling_level")) {
		if (*value == "disabled") g_Config.iTexScalingLevel = 1;
		else if (*value == "2x") g_Config.iTexScalingLevel = 2;
		else if (*value == "3x") g_Config.iTexScalingLevel = 3;
		else if (*value == "4x") g_Config.iTexScalingLevel = 4;
		else if (*value == "5x") g_Config.iTexScalingLevel = 5;
		else g_Config.iTexScalingLevel = std::clamp(OptionInt(*value, g_Config.iTexScalingLevel), 1, 5);
	}
	if (const std::string *value = FindOption(options, "ppsspp_texture_shader")) {
		if (*value == "disabled" || *value == "Off") g_Config.sTextureShaderName = "Off";
		else if (*value == "2xBRZ") g_Config.sTextureShaderName = "Tex2xBRZ";
		else if (*value == "4xBRZ") g_Config.sTextureShaderName = "Tex4xBRZ";
		else if (*value == "MMPX") g_Config.sTextureShaderName = "TexMMPX";
	}
	if (const std::string *value = FindOption(options, "ppsspp_texture_anisotropic_filtering")) {
		if (*value == "disabled" || *value == "Off") g_Config.iAnisotropyLevel = 0;
		else if (*value == "2x") g_Config.iAnisotropyLevel = 1;
		else if (*value == "4x") g_Config.iAnisotropyLevel = 2;
		else if (*value == "8x") g_Config.iAnisotropyLevel = 3;
		else if (*value == "16x") g_Config.iAnisotropyLevel = 4;
	}
	if (const std::string *value = FindOption(options, "ppsspp_texture_filtering")) {
		if (*value == "Auto") g_Config.iTexFiltering = 1;
		else if (*value == "Nearest") g_Config.iTexFiltering = 2;
		else if (*value == "Linear") g_Config.iTexFiltering = 3;
		else if (*value == "Auto max quality") g_Config.iTexFiltering = 4;
	}
	if (const std::string *value = FindOption(options, "ppsspp_language")) {
		if (*value == "Automatic") g_Config.iLanguage = -1;
		else if (*value == "English") g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
		else if (*value == "Japanese") g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_JAPANESE;
		else if (*value == "French") g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_FRENCH;
		else if (*value == "Spanish") g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_SPANISH;
		else if (*value == "German") g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_GERMAN;
		else if (*value == "Italian") g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_ITALIAN;
		else if (*value == "Dutch") g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_DUTCH;
		else if (*value == "Portuguese") g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_PORTUGUESE;
		else if (*value == "Russian") g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_RUSSIAN;
		else if (*value == "Korean") g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_KOREAN;
		else if (*value == "Chinese Traditional") g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_CHINESE_TRADITIONAL;
		else if (*value == "Chinese Simplified") g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_CHINESE_SIMPLIFIED;
	}

	g_Config.bTexHardwareScaling = g_Config.sTextureShaderName != "Off";
}

DisplayMode ParseDisplayMode(const std::string &value) {
	return value == "Integer" ? DisplayMode::Integer : DisplayMode::Display;
}

DisplaySize ParseDisplaySize(const std::string &value, DisplayMode mode) {
	if (value == "Stretch") return DisplaySize::Stretch;
	if (value == "4:3") return DisplaySize::_4_3;
	if (value == "16:9") return DisplaySize::_16_9;
	if (value == "Original") return DisplaySize::Original;
	if (value == "1x") return DisplaySize::_1x;
	if (value == "2x") return DisplaySize::_2x;
	if (value == "3x") return DisplaySize::_3x;
	if (value == "4x") return DisplaySize::_4x;
	if (value == "Auto") return DisplaySize::Auto;
	return mode == DisplayMode::Integer ? DisplaySize::Auto : DisplaySize::_16_9;
}

DisplaySize NormalizeDisplaySize(DisplayMode mode, DisplaySize size) {
	if (mode == DisplayMode::Integer) {
		switch (size) {
		case DisplaySize::_1x:
		case DisplaySize::_2x:
		case DisplaySize::_3x:
		case DisplaySize::_4x:
		case DisplaySize::Auto:
			return size;
		default:
			return DisplaySize::Auto;
		}
	}

	switch (size) {
	case DisplaySize::Stretch:
	case DisplaySize::_4_3:
	case DisplaySize::_16_9:
	case DisplaySize::Original:
		return size;
	default:
		return DisplaySize::_16_9;
	}
}

DisplaySettings DisplaySettingsFromOptions(const std::map<std::string, std::string> &options) {
	DisplaySettings settings;
	if (const std::string *value = FindOption(options, "display_mode")) {
		settings.mode = ParseDisplayMode(*value);
	}

	const std::string *sizeValue = nullptr;
	if (settings.mode == DisplayMode::Integer) {
		sizeValue = FindOption(options, "integer_scale");
	}
	if (!sizeValue) {
		sizeValue = FindOption(options, "display_size");
	}
	if (sizeValue) {
		settings.size = ParseDisplaySize(*sizeValue, settings.mode);
	}
	settings.size = NormalizeDisplaySize(settings.mode, settings.size);
	return settings;
}

const char *DisplayModeConfigValue(DisplayMode mode) {
	return mode == DisplayMode::Integer ? "Integer" : "Display";
}

const char *DisplaySizeConfigValue(DisplaySize size) {
	switch (size) {
	case DisplaySize::Stretch: return "Stretch";
	case DisplaySize::_4_3: return "4:3";
	case DisplaySize::_16_9: return "16:9";
	case DisplaySize::Original: return "Original";
	case DisplaySize::_1x: return "1x";
	case DisplaySize::_2x: return "2x";
	case DisplaySize::_3x: return "3x";
	case DisplaySize::_4x: return "4x";
	case DisplaySize::Auto: return "Auto";
	default: return "16:9";
	}
}

float NativeAspect() {
	return kPspNativeWidth / kPspNativeHeight;
}

float AspectAdjust(float desiredAspect) {
	return desiredAspect / NativeAspect();
}

float ScaleForNativeMultiple(int multiple) {
	const float screenW = (float)std::max(1, g_display.pixel_xres);
	const float screenH = (float)std::max(1, g_display.pixel_yres);
	const float nativeAspect = NativeAspect();

	float fittedW = screenW;
	float fittedH = fittedW / nativeAspect;
	if (fittedH > screenH) {
		fittedH = screenH;
		fittedW = fittedH * nativeAspect;
	}

	const float targetW = kPspNativeWidth * (float)multiple;
	const float targetH = kPspNativeHeight * (float)multiple;
	const float scale = std::min(targetW / fittedW, targetH / fittedH);
	return std::clamp(scale, 0.01f, 1.0f);
}

int MaxNativeMultiple() {
	const int screenW = std::max(1, g_display.pixel_xres);
	const int screenH = std::max(1, g_display.pixel_yres);
	return std::clamp(std::min(screenW / (int)kPspNativeWidth, screenH / (int)kPspNativeHeight), 1, 4);
}

int IntegerScaleValue(DisplaySize size) {
	switch (size) {
	case DisplaySize::_1x: return 1;
	case DisplaySize::_2x: return 2;
	case DisplaySize::_3x: return 3;
	case DisplaySize::_4x: return 4;
	default: return 0;
	}
}

void ApplySwitchRequiredConfig(bool audioReady) {
	g_Config.bFirstRun = false;
	g_Config.iCpuCore = (int)CPUCore::JIT;
	g_Config.bFastMemory = true;
	g_Config.bForceLagSync = false;
	g_Config.bSoftwareRendering = false;
	g_Config.iGPUBackend = (int)GPUBackend::VULKAN;
	// Keep emulation on the main thread and let the Vulkan backend submit work
	// from a separate thread on the other available application cores.
	g_Config.bRenderMultiThreading = true;
	g_Config.bEnableLogging = Logging::Enabled;
	g_Config.bEnableFileLogging = Logging::Enabled;
	g_Config.bEnableSound = audioReady;
	g_Config.bExtraAudioBuffering = true;
	g_Config.bDiscordRichPresence = false;
	g_Config.bAchievementsEnable = false;
	g_Config.bAchievementsEnableRAIntegration = false;
	g_Config.bAchievementsSoundEffects = false;
	g_Config.bEnableWlan = false;
	g_Config.bEnableAdhocServer = false;
	g_Config.bEnableNetworkChat = false;
	g_Config.bEnableUPnP = false;
	g_Config.sReportHost.clear();
	g_Config.internalDataDirectory = Path(kPpssppDataRoot);
	g_Config.memStickDirectory = Path(kPpssppDataRoot);
	g_Config.memStickSavedataDirectory = Path(kPpssppSaveDataRoot);
	g_Config.saveStateDirectory = Path(Paths::PpssppSaveStates);
	g_Config.iSaveStateSlotCount = Ppsspp::SaveStateSlotCount;
	g_Config.iCurrentStateSlot = std::clamp(g_Config.iCurrentStateSlot, 0, Ppsspp::SaveStateSlotCount - 1);
	g_Config.flash0Directory = Path(kPpssppDataRoot) / "flash0";
}

}  // namespace

PpssppCoreConfig::PpssppCoreConfig(LogCallback log)
	: config_("ppsspp", kPpssppCoreConfigPath, kDefaultPpssppCoreConfig, std::move(log)) {
}

void PpssppCoreConfig::Load() {
	config_.Load();
}

void PpssppCoreConfig::Apply(bool audioReady) const {
	const DisplaySettings displaySettings = DisplaySettingsFromOptions(config_.Options());
	ApplyPpssppOptions(config_.Options());
	ApplySwitchRequiredConfig(audioReady);
	ApplyPpssppDisplaySettings(displaySettings);
}

DisplaySettings LoadPpssppDisplaySettings(LogCallback log) {
	CoreConfig config("ppsspp", kPpssppCoreConfigPath, kDefaultPpssppCoreConfig, std::move(log));
	config.Load();
	return NormalizePpssppDisplaySettingsForCurrentMode(DisplaySettingsFromOptions(config.Options()));
}

void SavePpssppDisplaySettings(const DisplaySettings &settings, LogCallback log) {
	CoreConfig config("ppsspp", kPpssppCoreConfigPath, kDefaultPpssppCoreConfig, std::move(log));
	config.Load();
	const DisplaySettings normalizedSettings = NormalizePpssppDisplaySettingsForCurrentMode(settings);
	const DisplaySize normalizedSize = normalizedSettings.size;
	config.SetValue("display_mode", DisplayModeConfigValue(normalizedSettings.mode));
	config.SetValue("display_size", DisplaySizeConfigValue(normalizedSize));
	if (normalizedSettings.mode == DisplayMode::Integer) {
		config.SetValue("integer_scale", DisplaySizeConfigValue(normalizedSize));
	} else if (config.GetValue("integer_scale").empty()) {
		config.SetValue("integer_scale", DisplaySizeConfigValue(DisplaySize::Auto));
	}
	config.Save();
}

DisplaySettings NormalizePpssppDisplaySettingsForCurrentMode(const DisplaySettings &settings) {
	DisplaySettings normalized = settings;
	normalized.size = NormalizeDisplaySize(normalized.mode, normalized.size);
	if (normalized.mode == DisplayMode::Integer) {
		const int scale = IntegerScaleValue(normalized.size);
		if (scale > MaxNativeMultiple()) {
			normalized.size = DisplaySize::Auto;
		}
	}
	return normalized;
}

void ApplyPpssppDisplaySettings(const DisplaySettings &settings) {
	DisplayLayoutConfig &layout = g_Config.GetDisplayLayoutConfig(DeviceOrientation::Landscape);
	const DisplaySettings normalizedSettings = NormalizePpssppDisplaySettingsForCurrentMode(settings);
	const DisplaySize normalizedSize = normalizedSettings.size;

	layout.bDisplayStretch = false;
	layout.bDisplayIntegerScale = false;
	layout.fDisplayOffsetX = 0.5f;
	layout.fDisplayOffsetY = 0.5f;
	layout.fDisplayScale = 1.0f;
	layout.fDisplayAspectRatio = 1.0f;

	if (normalizedSettings.mode == DisplayMode::Integer) {
		int scale = MaxNativeMultiple();
		if (normalizedSize == DisplaySize::_1x) scale = 1;
		else if (normalizedSize == DisplaySize::_2x) scale = 2;
		else if (normalizedSize == DisplaySize::_3x) scale = 3;
		else if (normalizedSize == DisplaySize::_4x) scale = 4;
		layout.fDisplayScale = ScaleForNativeMultiple(scale);
		return;
	}

	switch (normalizedSize) {
	case DisplaySize::Stretch:
		layout.bDisplayStretch = true;
		break;
	case DisplaySize::_4_3:
		layout.fDisplayAspectRatio = AspectAdjust(4.0f / 3.0f);
		break;
	case DisplaySize::_16_9:
		layout.fDisplayAspectRatio = AspectAdjust(16.0f / 9.0f);
		break;
	case DisplaySize::Original:
	default:
		layout.fDisplayAspectRatio = 1.0f;
		break;
	}
}

int MaxPpssppIntegerScaleForCurrentDisplay() {
	return MaxNativeMultiple();
}

const char *DisplayModeLabel(DisplayMode mode) {
	return mode == DisplayMode::Integer ? "Integer" : "Display";
}

const char *DisplaySizeLabel(DisplaySize size) {
	return DisplaySizeConfigValue(size);
}

}  // namespace Tico
