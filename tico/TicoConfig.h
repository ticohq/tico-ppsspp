#pragma once

namespace Tico {

namespace Paths {
constexpr const char *Root = "sdmc:/tico";
constexpr const char *Debug = "sdmc:/tico/debug";
constexpr const char *Assets = "sdmc:/tico/assets";
constexpr const char *Lang = "sdmc:/tico/lang";
constexpr const char *System = "sdmc:/tico/system";
constexpr const char *CoreConfigDir = "sdmc:/tico/config/cores";
constexpr const char *PpssppCoreConfig = "sdmc:/tico/config/cores/ppsspp.jsonc";
constexpr const char *PpssppDataRoot = "sdmc:/tico/system/psp";
constexpr const char *SavesRoot = "sdmc:/tico/saves";
constexpr const char *PpssppSaveDataRoot = "sdmc:/tico/saves/psp";
constexpr const char *StatesRoot = "sdmc:/tico/states";
constexpr const char *PpssppSaveStates = "sdmc:/tico/states/psp";
constexpr const char *BootLog = "sdmc:/switch/ppsspp-minimal.log";
constexpr const char *LauncherNro = "sdmc:/switch/tico.nro";
constexpr const char *LauncherNroFallback = "sdmc:/tico/tico.nro";
constexpr const char *DefaultTitleFont = "romfs:/fonts/font.ttf";
constexpr const char *DefaultDescriptionFont = "romfs:/fonts/description.ttf";
}  // namespace Paths

namespace Display {
constexpr int Width = 1280;
constexpr int Height = 720;
constexpr float FontSize = 32.0f;
}  // namespace Display

namespace Ppsspp {
constexpr int SaveStateSlotCount = 4;
}  // namespace Ppsspp

namespace Logging {
constexpr bool Enabled = false;
}  // namespace Logging

}  // namespace Tico
