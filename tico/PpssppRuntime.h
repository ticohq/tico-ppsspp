#pragma once

#include "tico/TicoMain.h"

namespace Tico {

class PpssppRuntime final : public CoreRuntime {
public:
	explicit PpssppRuntime(LogCallback log = {});
	~PpssppRuntime() override;

	const char *Name() const override { return "ppsspp"; }
	bool Configure(const LaunchInfo &launch) override;
	bool Initialize(const LaunchInfo &launch) override;
	bool LoadContent(const std::string &path) override;
	void HandleInput(const FrameInput &input) override;
	void RunFrame() override;
	void RenderFrame() override;
	bool ShouldExit() const override;
	bool ShouldChainloadLauncher() const override;
	void RequestExit() override;
	void Shutdown() override;

private:
	LogCallback log_;
};

}  // namespace Tico
