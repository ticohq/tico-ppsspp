#pragma once

#include "Common/CommonTypes.h"
#include "tico/TicoCoreConfig.h"

#include <string>

namespace Tico {

struct LaunchInfo {
	int argc = 0;
	char **argv = nullptr;
	std::string contentPath;
};

struct FrameInput {
	u64 buttons = 0;
	u64 pressed = 0;
	u64 released = 0;
	int leftStickX = 0;
	int leftStickY = 0;
	int rightStickX = 0;
	int rightStickY = 0;
};

class CoreRuntime {
public:
	virtual ~CoreRuntime() = default;

	virtual const char *Name() const = 0;
	virtual bool Configure(const LaunchInfo &) { return true; }
	virtual bool Initialize(const LaunchInfo &) = 0;
	virtual bool LoadContent(const std::string &path) = 0;
	virtual void HandleInput(const FrameInput &) {}
	virtual void RunFrame() = 0;
	virtual void RenderFrame() = 0;
	virtual bool ShouldExit() const = 0;
	virtual bool ShouldChainloadLauncher() const { return false; }
	virtual void RequestExit() = 0;
	virtual void Shutdown() = 0;
};

class Main {
public:
	explicit Main(CoreRuntime &runtime, LogCallback log = {});

	int Run(int argc, char **argv);

private:
	bool InitPlatform();
	void ShutdownPlatform();
	FrameInput PollInput();
	void Log(const char *fmt, ...) const;

	CoreRuntime &runtime_;
	LogCallback log_;
	bool platformReady_ = false;
	bool exitLocked_ = false;
	bool socketReady_ = false;
};

}  // namespace Tico
