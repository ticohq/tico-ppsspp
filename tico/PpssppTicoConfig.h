#pragma once

#include <string>

#include "tico/TicoConfig.h"
#include "tico/TicoCoreConfig.h"

namespace Tico {

constexpr const char *kPpssppDataRoot = Paths::PpssppDataRoot;
constexpr const char *kPpssppSaveDataRoot = Paths::PpssppSaveDataRoot;
constexpr const char *kPpssppCoreConfigPath = Paths::PpssppCoreConfig;

/// The current user's PSP saves and states (tico's folders, or theirs in
/// them), from tico's session.
std::string UserSaveDataRoot();
std::string UserSaveStates();

enum class DisplayMode {
	Integer = 0,
	Display = 1,
	COUNT = 2,
};

enum class DisplaySize {
	Stretch = 0,
	_4_3 = 1,
	_16_9 = 2,
	Original = 3,
	_1x = 4,
	_2x = 5,
	_3x = 6,
	_4x = 7,
	Auto = 8,
};

struct DisplaySettings {
	DisplayMode mode = DisplayMode::Display;
	DisplaySize size = DisplaySize::_16_9;
};

class PpssppCoreConfig {
public:
	explicit PpssppCoreConfig(LogCallback log = {});

	void Load();
	void Apply(bool audioReady) const;
	// Call after Apply(): keeps a generated MAC stable across launches.
	void PersistGeneratedMacAddress();

	const CoreConfig &RawConfig() const { return config_; }
	CoreConfig &RawConfig() { return config_; }

private:
	CoreConfig config_;
};

// Frame generation (LSFG, Vulkan): ppsspp_lsfg, ppsspp_lsfg_flow_scale
// ("0.25"/"0.5") and ppsspp_lsfg_performance_mode, as applied at launch.
struct LsfgSettings {
	bool enabled = false;
	float flowScale = 0.25f;
	bool performanceMode = true;
};
const LsfgSettings &PpssppLsfgSettings();

DisplaySettings LoadPpssppDisplaySettings(LogCallback log = {});
void SavePpssppDisplaySettings(const DisplaySettings &settings, LogCallback log = {});
DisplaySettings NormalizePpssppDisplaySettingsForCurrentMode(const DisplaySettings &settings);
void ApplyPpssppDisplaySettings(const DisplaySettings &settings);
int MaxPpssppIntegerScaleForCurrentDisplay();
const char *DisplayModeLabel(DisplayMode mode);
const char *DisplaySizeLabel(DisplaySize size);

}  // namespace Tico
