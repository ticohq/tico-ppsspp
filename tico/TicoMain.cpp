#include "TicoMain.h"

#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(SWITCH)

#include "tico/TicoChainload.h"
#include "tico/TicoConfig.h"

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <csignal>
#include <sys/stat.h>
#include <utility>

extern "C" {
u32 __NvOptimusEnablement = 1;
u32 __NvDeveloperOption = 1;
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = 0x8000;
}

namespace Tico {

namespace {
PadState g_pad;
}

Main::Main(CoreRuntime &runtime, LogCallback log) : runtime_(runtime), log_(std::move(log)) {
}

int Main::Run(int argc, char **argv) {
	LaunchInfo launch{};
	launch.argc = argc;
	launch.argv = argv;
	if (argc > 1 && argv[1]) {
		launch.contentPath = argv[1];
	}

	if (launch.contentPath.empty()) {
		std::fprintf(stderr, "Usage: %s <content path>\n", argc > 0 && argv[0] ? argv[0] : runtime_.Name());
		return 1;
	}

	if (!InitPlatform()) {
		return 1;
	}

	Log("tico main start core=%s argc=%d content=%s", runtime_.Name(), argc, launch.contentPath.c_str());
	bool started = runtime_.Configure(launch) && runtime_.Initialize(launch) && runtime_.LoadContent(launch.contentPath);
	if (!started) {
		Log("tico main init failed core=%s", runtime_.Name());
		runtime_.Shutdown();
		ShutdownPlatform();
		return 1;
	}

	Log("tico main loop enter core=%s", runtime_.Name());
	while (appletMainLoop() && !runtime_.ShouldExit()) {
		const FrameInput input = PollInput();
		runtime_.HandleInput(input);
		runtime_.RunFrame();
		runtime_.RenderFrame();
	}
	Log("tico main loop exit core=%s", runtime_.Name());

	const bool shouldChainloadLauncher = runtime_.ShouldChainloadLauncher();
	runtime_.Shutdown();
	ShutdownPlatform();
	if (shouldChainloadLauncher) {
		ChainloadLauncher(log_);
	}
	return 0;
}

bool Main::InitPlatform() {
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		std::perror("Unable to ignore SIGPIPE");
	}

	mkdir(Paths::Root, 0777);
	mkdir(Paths::Debug, 0777);
	mkdir(Paths::Assets, 0777);
	mkdir(Paths::Lang, 0777);
	mkdir(Paths::System, 0777);
	mkdir(Paths::PpssppDataRoot, 0777);
	mkdir(Paths::SavesRoot, 0777);
	mkdir(Paths::PpssppSaveDataRoot, 0777);
	mkdir("sdmc:/tico/config", 0777);
	mkdir(Paths::CoreConfigDir, 0777);
	mkdir(Paths::StatesRoot, 0777);
	mkdir(Paths::PpssppSaveStates, 0777);
	mkdir("sdmc:/tico/assets/ra", 0777);

	appletLockExit();
	exitLocked_ = true;

	const Result socketRc = socketInitializeDefault();
	if (R_FAILED(socketRc)) {
		Log("socketInitializeDefault failed rc=0x%x", (unsigned)socketRc);
	} else {
		socketReady_ = true;
	}

	const Result romfsRc = romfsInit();
	if (R_FAILED(romfsRc)) {
		Log("romfsInit failed rc=0x%x", (unsigned)romfsRc);
		ShutdownPlatform();
		return false;
	}

	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&g_pad);
	platformReady_ = true;
	return true;
}

void Main::ShutdownPlatform() {
	if (platformReady_) {
		romfsExit();
		platformReady_ = false;
	}
	if (socketReady_) {
		socketExit();
		socketReady_ = false;
	}
	if (exitLocked_) {
		appletUnlockExit();
		exitLocked_ = false;
	}
}

FrameInput Main::PollInput() {
	padUpdate(&g_pad);
	FrameInput input{};
	input.buttons = padGetButtons(&g_pad);
	input.pressed = padGetButtonsDown(&g_pad);
	input.released = padGetButtonsUp(&g_pad);
	const HidAnalogStickState leftStick = padGetStickPos(&g_pad, 0);
	const HidAnalogStickState rightStick = padGetStickPos(&g_pad, 1);
	input.leftStickX = leftStick.x;
	input.leftStickY = leftStick.y;
	input.rightStickX = rightStick.x;
	input.rightStickY = rightStick.y;
	return input;
}

void Main::Log(const char *fmt, ...) const {
	if (!log_ || !fmt) {
		return;
	}

	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	log_(buffer);
}

}  // namespace Tico

#endif
