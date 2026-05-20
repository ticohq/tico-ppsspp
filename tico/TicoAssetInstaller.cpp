#include "tico/TicoAssetInstaller.h"

#include "tico/TicoConfig.h"

#include <switch.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace Tico {
namespace {

constexpr const char *kSourceAssetsRoot = "romfs:/assets";
constexpr const char *kAssetInstallMarker = "sdmc:/tico/system/psp/.tico_assets_version";
constexpr const char *kAssetInstallVersion = "ppsspp-tico-assets-v3";
constexpr size_t kCopyBufferSize = 64 * 1024;

constexpr std::array<const char *, 4> kRomfsOnlyTicoAssets = {{
	"bolt.svg",
	"loader.svg",
	"ra.svg",
	"trophy.mp3",
}};

// Sentinel files only: these make startup validation cheap. The installer still
// copies the whole PPSSPP asset tree recursively when this check fails.
constexpr std::array<const char *, 5> kRequiredAssetSentinelFiles = {{
	"sdmc:/tico/system/psp/flash0/font/ltn0.pgf",
	"sdmc:/tico/system/psp/vfpu/vfpu_sqrt_lut.dat",
	"sdmc:/tico/system/psp/lang/en_US.ini",
	"sdmc:/tico/system/psp/ppge_atlas.zim",
	"sdmc:/tico/system/psp/compat.ini",
}};

bool StatPath(const std::string &path, struct stat *out) {
	return stat(path.c_str(), out) == 0;
}

bool SdFileExistsFast(const std::string &path) {
	struct stat st {};
	return StatPath(path, &st) && S_ISREG(st.st_mode) && st.st_size > 0;
}

bool SdDirectoryExistsFast(const std::string &path) {
	struct stat st {};
	return StatPath(path, &st) && S_ISDIR(st.st_mode);
}

std::string JoinPath(const std::string &base, const std::string &name) {
	if (base.empty() || base.back() == '/') {
		return base + name;
	}
	return base + "/" + name;
}

bool IsRomfsOnlyTicoAsset(const std::string &source, const char *name) {
	if (source != kSourceAssetsRoot) {
		return false;
	}
	for (const char *ticoAsset : kRomfsOnlyTicoAssets) {
		if (!std::strcmp(name, ticoAsset)) {
			return true;
		}
	}
	return false;
}

bool EnsureDirectory(const std::string &path) {
	if (path.empty() || SdDirectoryExistsFast(path)) {
		return true;
	}

	return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST || SdDirectoryExistsFast(path);
}

bool CopyFile(const std::string &source, const std::string &destination, std::vector<unsigned char> *buffer, const LogCallback &log) {
	FILE *in = fopen(source.c_str(), "rb");
	if (!in) {
		LogMessage(log, "asset install open source failed %s", source.c_str());
		return false;
	}

	remove(destination.c_str());
	FILE *out = fopen(destination.c_str(), "wb");
	if (!out) {
		fclose(in);
		LogMessage(log, "asset install open destination failed %s errno=%d", destination.c_str(), errno);
		return false;
	}

	bool ok = true;
	while (!feof(in)) {
		const size_t readSize = fread(buffer->data(), 1, buffer->size(), in);
		if (readSize > 0) {
			const size_t writeSize = fwrite(buffer->data(), 1, readSize, out);
			if (writeSize != readSize) {
				ok = false;
				break;
			}
		}
		if (ferror(in) || ferror(out)) {
			ok = false;
			break;
		}
	}

	if (fflush(out) != 0) {
		ok = false;
	}
	if (fclose(out) != 0) {
		ok = false;
	}
	fclose(in);

	if (!ok) {
		remove(destination.c_str());
		LogMessage(log, "asset install copy failed %s -> %s errno=%d", source.c_str(), destination.c_str(), errno);
	}
	return ok;
}

bool CopyDirectoryRecursive(const std::string &source, const std::string &destination, std::vector<unsigned char> *buffer, const LogCallback &log) {
	if (!EnsureDirectory(destination)) {
		LogMessage(log, "asset install mkdir failed %s", destination.c_str());
		return false;
	}

	DIR *dir = opendir(source.c_str());
	if (!dir) {
		LogMessage(log, "asset install opendir failed %s", source.c_str());
		return false;
	}

	bool ok = true;
	struct dirent *entry = nullptr;
	while ((entry = readdir(dir)) != nullptr) {
		if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) {
			continue;
		}
		if (IsRomfsOnlyTicoAsset(source, entry->d_name)) {
			continue;
		}

		const std::string sourcePath = JoinPath(source, entry->d_name);
		const std::string destinationPath = JoinPath(destination, entry->d_name);
		struct stat st {};
		if (!StatPath(sourcePath, &st)) {
			LogMessage(log, "asset install stat failed %s", sourcePath.c_str());
			ok = false;
			break;
		}

		if (S_ISDIR(st.st_mode)) {
			if (!CopyDirectoryRecursive(sourcePath, destinationPath, buffer, log)) {
				ok = false;
				break;
			}
		} else if (S_ISREG(st.st_mode)) {
			if (!CopyFile(sourcePath, destinationPath, buffer, log)) {
				ok = false;
				break;
			}
		}
	}

	closedir(dir);
	return ok;
}

bool MarkerMatches() {
	FILE *file = fopen(kAssetInstallMarker, "rb");
	if (!file) {
		return false;
	}

	char buffer[64] {};
	const size_t readSize = fread(buffer, 1, sizeof(buffer) - 1, file);
	fclose(file);
	if (readSize == 0) {
		return false;
	}

	std::string value(buffer, readSize);
	while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
		value.pop_back();
	}
	return value == kAssetInstallVersion;
}

bool AssetsComplete() {
	if (!MarkerMatches()) {
		return false;
	}
	for (const char *path : kRequiredAssetSentinelFiles) {
		if (!SdFileExistsFast(path)) {
			return false;
		}
	}
	return true;
}

bool WriteMarker(const LogCallback &log) {
	const std::string marker = std::string(kAssetInstallVersion) + "\n";
	remove(kAssetInstallMarker);
	FILE *file = fopen(kAssetInstallMarker, "wb");
	if (!file) {
		LogMessage(log, "asset install marker open failed %s errno=%d", kAssetInstallMarker, errno);
		return false;
	}
	const size_t written = fwrite(marker.data(), 1, marker.size(), file);
	const bool flushed = fflush(file) == 0;
	const bool closed = fclose(file) == 0;
	const bool ok = written == marker.size() && flushed && closed;
	if (!ok) {
		remove(kAssetInstallMarker);
		LogMessage(log, "asset install marker write failed %s errno=%d", kAssetInstallMarker, errno);
		return false;
	}
	return true;
}

void RemoveStaleTicoAssetsFromPpssppRoot(const LogCallback &log) {
	for (const char *asset : kRomfsOnlyTicoAssets) {
		const std::string path = JoinPath(Paths::PpssppDataRoot, asset);
		if (remove(path.c_str()) == 0) {
			LogMessage(log, "asset install removed stale tico asset %s", path.c_str());
		}
	}
}

}  // namespace

bool InstallPpssppAssets(LogCallback log) {
	// Sentinel checks keep normal launches fast without walking RomFS.
	if (AssetsComplete()) {
		LogMessage(log, "asset install skipped version=%s", kAssetInstallVersion);
		return true;
	}

	struct stat sourceSt {};
	if (!StatPath(kSourceAssetsRoot, &sourceSt) || !S_ISDIR(sourceSt.st_mode)) {
		LogMessage(log, "asset install missing source %s", kSourceAssetsRoot);
		return false;
	}

	EnsureDirectory(Paths::Root);
	EnsureDirectory(Paths::System);
	if (!EnsureDirectory(Paths::PpssppDataRoot)) {
		LogMessage(log, "asset install destination unavailable %s", Paths::PpssppDataRoot);
		return false;
	}

	LogMessage(log, "asset install start %s -> %s", kSourceAssetsRoot, Paths::PpssppDataRoot);
	std::vector<unsigned char> buffer(kCopyBufferSize);
	if (!CopyDirectoryRecursive(kSourceAssetsRoot, Paths::PpssppDataRoot, &buffer, log)) {
		return false;
	}
	RemoveStaleTicoAssetsFromPpssppRoot(log);
	if (!WriteMarker(log)) {
		return false;
	}
	fsdevCommitDevice("sdmc");

	LogMessage(log, "asset install complete version=%s", kAssetInstallVersion);
	return true;
}

}  // namespace Tico
