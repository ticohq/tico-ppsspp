#pragma once

#include <cctype>
#include <string>

namespace Tico {

inline std::string Trim(const std::string &value) {
	size_t begin = 0;
	while (begin < value.size() && std::isspace((unsigned char)value[begin])) {
		++begin;
	}

	size_t end = value.size();
	while (end > begin && std::isspace((unsigned char)value[end - 1])) {
		--end;
	}

	return value.substr(begin, end - begin);
}

inline std::string GetCleanTitle(const std::string &filename) {
	std::string title(filename);
	const size_t dot = title.find_last_of('.');
	if (dot != std::string::npos && dot > 0) {
		title = title.substr(0, dot);
	}

	std::string result;
	result.reserve(title.size());
	int parenDepth = 0;
	int bracketDepth = 0;
	bool lastWasSpace = false;

	for (char c : title) {
		if (c == '(') {
			++parenDepth;
			continue;
		}
		if (c == ')') {
			if (parenDepth > 0) {
				--parenDepth;
			}
			continue;
		}
		if (c == '[') {
			++bracketDepth;
			continue;
		}
		if (c == ']') {
			if (bracketDepth > 0) {
				--bracketDepth;
			}
			continue;
		}

		if (parenDepth == 0 && bracketDepth == 0) {
			if (c == ' ' || c == '_') {
				if (!result.empty() && !lastWasSpace) {
					result += ' ';
					lastWasSpace = true;
				}
			} else {
				result += c;
				lastWasSpace = false;
			}
		}
	}

	return Trim(result);
}

inline std::string GameTitleFromPath(const char *path) {
	if (!path || !path[0]) {
		return "PPSSPP";
	}

	std::string filename(path);
	const size_t slash = filename.find_last_of("/\\");
	if (slash != std::string::npos) {
		filename = filename.substr(slash + 1);
	}

	std::string title = GetCleanTitle(filename);
	if (title.empty()) {
		const size_t dot = filename.find_last_of('.');
		if (dot != std::string::npos && dot > 0) {
			filename = filename.substr(0, dot);
		}
		title = Trim(filename);
	}
	return title.empty() ? "PPSSPP" : title;
}

}  // namespace Tico
