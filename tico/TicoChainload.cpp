#include "TicoChainload.h"

#include "ppsspp_config.h"

#include "tico/TicoConfig.h"

#if PPSSPP_PLATFORM(SWITCH)
#include <switch.h>
#endif

#include <cstdio>
#include <sys/stat.h>

namespace Tico {
namespace {

bool FileExists(const char *path) {
	struct stat st {};
	return path && stat(path, &st) == 0;
}

const char *FindLauncherNro() {
	const char *candidates[] = {
		Paths::LauncherNro,
		Paths::LauncherNroFallback,
	};
	for (const char *candidate : candidates) {
		if (FileExists(candidate)) {
			return candidate;
		}
	}
	return nullptr;
}

}  // namespace

bool ChainloadLauncher(LogCallback log) {
#if PPSSPP_PLATFORM(SWITCH)
	const char *nextNro = FindLauncherNro();
	if (!nextNro) {
		LogMessage(log, "tico chainload skipped missing targets=%s,%s", Paths::LauncherNro, Paths::LauncherNroFallback);
		return false;
	}

	char args[512];
	std::snprintf(args, sizeof(args), "%s --resume", nextNro);
	envSetNextLoad(nextNro, args);
	LogMessage(log, "tico chainload next=%s args=%s", nextNro, args);
	return true;
#else
	LogMessage(log, "tico chainload skipped unsupported platform");
	return false;
#endif
}

}  // namespace Tico
