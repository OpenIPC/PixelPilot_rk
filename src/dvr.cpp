#include <string>
#include <filesystem>
#include <regex>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <algorithm>

#include "spdlog/spdlog.h"

#include "dvr.h"

namespace fs = std::filesystem;

// Master record flag; defined here, declared extern in dvr.h.
int dvr_enabled = 0;

const int SEQUENCE_PADDING = 4; // zero-padding width for --dvr-sequenced-files

std::string dvr_next_base_path(const char* filename_template, bool with_sequence) {
	if (!filename_template) return "";
	fs::path pathObj(filename_template);
	std::string rec_dir = pathObj.parent_path().string();
	std::string filename_pattern = pathObj.filename().string();
	if (rec_dir.empty()) rec_dir = ".";

	if (!fs::exists(rec_dir)) {
		spdlog::error("Error: Directory does not exist: {}", rec_dir);
		return "";
	}

	std::string paddedNumber;
	if (with_sequence) {
		std::regex pattern(R"(^(\d+)_.*)"); // filenames starting with digits then '_'
		int maxNumber = -1;
		for (const auto &entry : fs::directory_iterator(rec_dir)) {
			if (!entry.is_regular_file()) continue;
			std::string filename = entry.path().filename().string();
			std::smatch match;
			if (std::regex_match(filename, match, pattern)) {
				int number = std::stoi(match[1].str());
				maxNumber = std::max(maxNumber, number);
			}
		}
		int nextFileNumber = (maxNumber == -1) ? 0 : maxNumber + 1;
		std::ostringstream stream;
		stream << std::setw(SEQUENCE_PADDING) << std::setfill('0') << nextFileNumber;
		paddedNumber = stream.str() + "_";
	}

	std::time_t now = std::time(nullptr);
	std::tm *localTime = std::localtime(&now);
	char formattedFilename[256];
	std::strftime(formattedFilename, sizeof(formattedFilename), filename_pattern.c_str(), localTime);

	std::string finalFilename = rec_dir + "/" + paddedNumber + formattedFilename;
	if (finalFilename.size() >= 4 && finalFilename.substr(finalFilename.size() - 4) == ".mp4")
		return finalFilename.substr(0, finalFilename.size() - 4);
	return finalFilename;
}
