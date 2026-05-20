#pragma once

#include "Common/CommonTypes.h"
#include "tico/TicoCoreConfig.h"

#include <mutex>
#include <vector>

namespace Tico {

class AudioSfx {
public:
	bool Load(LogCallback log = {});
	void Shutdown();
	void PlayTrophy();
	void Mix(s16 *output, int frames, int outputRate);

private:
	bool LoadMp3File(const char *path);

	LogCallback log_;
	std::mutex mutex_;
	std::vector<s16> trophySamples_;
	int trophyChannels_ = 0;
	int trophySampleRate_ = 0;
	double trophyCursor_ = 0.0;
	bool trophyPlaying_ = false;
};

AudioSfx &TrophySfx();

}  // namespace Tico
