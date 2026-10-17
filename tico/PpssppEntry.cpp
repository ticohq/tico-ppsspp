#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(SWITCH)

#include "tico/PpssppRuntime.h"
#include "tico/TicoConfig.h"
#include "tico/TicoLogger.h"
#include "tico/TicoMain.h"
#include "dep/nlohmann/json.hpp" // TicoSession.h uses the one already included
#include "tico/TicoSession.h"

int main(int argc, char **argv) {
	// Mesa's shader cache beside this core's files, not at the SD card's root
	tico::UseShaderCacheDir("sdmc:/tico/system/ppsspp/cache/mesa");
	Tico::FileLogger logger;
	if constexpr (Tico::Logging::Enabled) {
		logger.Open(Tico::Paths::BootLog);
	}
	Tico::PpssppRuntime runtime(logger.Callback());
	Tico::Main app(runtime, logger.Callback());
	return app.Run(argc, argv);
}

#endif
