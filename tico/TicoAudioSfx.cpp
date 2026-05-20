#include "tico/TicoAudioSfx.h"

#include "ext/minimp3/minimp3_ex.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace Tico {
namespace {

constexpr float kTrophyVolume = 0.85f;

AudioSfx g_audioSfx;

bool ReadFileBytes(const char *path, std::vector<uint8_t> *data) {
	data->clear();
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		return false;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return false;
	}
	const long size = ftell(fp);
	if (size <= 0) {
		fclose(fp);
		return false;
	}
	rewind(fp);
	data->resize((size_t)size);
	const size_t readSize = fread(data->data(), 1, data->size(), fp);
	fclose(fp);
	if (readSize != data->size()) {
		data->clear();
		return false;
	}
	return true;
}

inline s16 Clamp16(int sample) {
	if (sample < -32768) {
		return -32768;
	}
	if (sample > 32767) {
		return 32767;
	}
	return (s16)sample;
}

}  // namespace

AudioSfx &TrophySfx() {
	return g_audioSfx;
}

bool AudioSfx::Load(LogCallback log) {
	log_ = std::move(log);
	Shutdown();

	if (LoadMp3File("romfs:/assets/trophy.mp3")) {
		LogMessage(log_, "tico sfx loaded romfs:/assets/trophy.mp3 rate=%d channels=%d samples=%zu",
			trophySampleRate_, trophyChannels_, trophySamples_.size());
		return true;
	}
	if (LoadMp3File("sdmc:/tico/assets/trophy.mp3")) {
		LogMessage(log_, "tico sfx loaded sdmc:/tico/assets/trophy.mp3 rate=%d channels=%d samples=%zu",
			trophySampleRate_, trophyChannels_, trophySamples_.size());
		return true;
	}

	LogMessage(log_, "tico sfx trophy.mp3 not found");
	return false;
}

void AudioSfx::Shutdown() {
	std::lock_guard<std::mutex> lock(mutex_);
	trophySamples_.clear();
	trophyChannels_ = 0;
	trophySampleRate_ = 0;
	trophyCursor_ = 0.0;
	trophyPlaying_ = false;
}

void AudioSfx::PlayTrophy() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (trophySamples_.empty() || trophyChannels_ <= 0 || trophySampleRate_ <= 0) {
		return;
	}
	trophyCursor_ = 0.0;
	trophyPlaying_ = true;
}

void AudioSfx::Mix(s16 *output, int frames, int outputRate) {
	if (!output || frames <= 0 || outputRate <= 0) {
		return;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	if (!trophyPlaying_ || trophySamples_.empty() || trophyChannels_ <= 0 || trophySampleRate_ <= 0) {
		return;
	}

	const int totalFrames = (int)trophySamples_.size() / trophyChannels_;
	const double step = (double)trophySampleRate_ / (double)outputRate;
	for (int i = 0; i < frames; ++i) {
		const int sourceFrame = (int)trophyCursor_;
		if (sourceFrame >= totalFrames) {
			trophyPlaying_ = false;
			trophyCursor_ = 0.0;
			break;
		}

		const int sourceIndex = sourceFrame * trophyChannels_;
		const int left = trophySamples_[sourceIndex];
		const int right = trophyChannels_ > 1 ? trophySamples_[sourceIndex + 1] : left;
		const int mixedLeft = output[i * 2] + (int)((float)left * kTrophyVolume);
		const int mixedRight = output[i * 2 + 1] + (int)((float)right * kTrophyVolume);
		output[i * 2] = Clamp16(mixedLeft);
		output[i * 2 + 1] = Clamp16(mixedRight);
		trophyCursor_ += step;
	}
}

bool AudioSfx::LoadMp3File(const char *path) {
	std::vector<uint8_t> data;
	if (!ReadFileBytes(path, &data)) {
		return false;
	}

	mp3dec_t decoder;
	mp3dec_init(&decoder);
	mp3dec_file_info_t info{};
	const int result = mp3dec_load_buf(&decoder, data.data(), data.size(), &info, nullptr, nullptr);
	if (result < 0 || info.samples == 0 || !info.buffer || info.channels <= 0 || info.hz <= 0) {
		if (info.buffer) {
			free(info.buffer);
		}
		LogMessage(log_, "tico sfx failed to decode %s", path);
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		trophySamples_.assign(info.buffer, info.buffer + info.samples);
		trophyChannels_ = info.channels;
		trophySampleRate_ = info.hz;
		trophyCursor_ = 0.0;
		trophyPlaying_ = false;
	}
	free(info.buffer);
	return true;
}

}  // namespace Tico
