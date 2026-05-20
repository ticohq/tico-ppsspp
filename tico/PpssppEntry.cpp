#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(SWITCH)

#include "tico/PpssppRuntime.h"
#include "tico/TicoConfig.h"
#include "tico/TicoLogger.h"
#include "tico/TicoMain.h"

int main(int argc, char **argv) {
	Tico::FileLogger logger;
	if constexpr (Tico::Logging::Enabled) {
		logger.Open(Tico::Paths::BootLog);
	}
	Tico::PpssppRuntime runtime(logger.Callback());
	Tico::Main app(runtime, logger.Callback());
	return app.Run(argc, argv);
}

#endif
