#pragma once

#include "tico/TicoCoreConfig.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>

namespace Tico {

class FileLogger {
public:
	FileLogger() = default;
	explicit FileLogger(std::string path) {
		Open(std::move(path));
	}

	~FileLogger() {
		Close();
	}

	bool Open(std::string path) {
		Close();
		path_ = std::move(path);
		file_ = fopen(path_.c_str(), "w");
		return file_ != nullptr;
	}

	void Close() {
		if (file_) {
			fclose(file_);
			file_ = nullptr;
		}
	}

	void Log(const char *fmt, ...) {
		if (!file_ || !fmt) {
			return;
		}

		va_list args;
		va_start(args, fmt);
		vfprintf(file_, fmt, args);
		va_end(args);
		fputc('\n', file_);
		fflush(file_);
	}

	LogCallback Callback() {
		return [this](const std::string &message) {
			Log("%s", message.c_str());
		};
	}

private:
	std::string path_;
	FILE *file_ = nullptr;
};

}  // namespace Tico
