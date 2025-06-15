#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <iostream>
#include <filesystem>
#include <regex>

#include "spdlog/spdlog.h"

#include "dvr.h"
#include "minimp4.h"

#include "gstrtpreceiver.h"
extern "C" {
#include "osd.h"
}

namespace fs = std::filesystem;

int dvr_enabled = 0;
const int SEQUENCE_PADDING = 4; // Configurable padding for sequence numbers

int write_callback(int64_t offset, const void *buffer, size_t size, void *token){
	FILE *f = (FILE*)token;
	fseek(f, offset, SEEK_SET);
	return fwrite(buffer, 1, size, f) != size;
}

Dvr::Dvr(dvr_thread_params params) {
	filename_template = params.filename_template;
	mp4_fragmentation_mode = params.mp4_fragmentation_mode;
	dvr_filenames_with_sequence = params.dvr_filenames_with_sequence;
	video_framerate = params.video_framerate;
	video_frm_width = params.video_p.video_frm_width;
	video_frm_height = params.video_p.video_frm_height;
	codec = params.video_p.codec;
	dvr_file = NULL;
	mp4wr = (mp4_h26x_writer_t *)malloc(sizeof(mp4_h26x_writer_t));
}

Dvr::~Dvr() {}

void Dvr::frame(std::shared_ptr<std::vector<uint8_t>> frame) {
	dvr_rpc rpc = {
		.command = dvr_rpc::RPC_FRAME,
		.frame = frame
	};
	enqueue_dvr_command(rpc);
}

void Dvr::set_video_params(uint32_t video_frm_w,
						   uint32_t video_frm_h,
						   VideoCodec codec) {
	video_frm_width = video_frm_w;
	video_frm_height = video_frm_h;
	codec = codec;
	dvr_rpc rpc = {
		.command = dvr_rpc::RPC_SET_PARAMS
	};
	enqueue_dvr_command(rpc);
}

void Dvr::start_recording() {
	dvr_rpc rpc = {
		.command = dvr_rpc::RPC_START
	};
	enqueue_dvr_command(rpc);
}

void Dvr::stop_recording() {
	dvr_rpc rpc = {
		.command = dvr_rpc::RPC_STOP
	};
	enqueue_dvr_command(rpc);
}

void Dvr::set_video_framerate(int rate) {
	video_framerate = rate;
	spdlog::info("Changeing video framerate to {}",video_framerate);
}

void Dvr::toggle_recording() {
	dvr_rpc rpc = {
		.command = dvr_rpc::RPC_TOGGLE
	};
	enqueue_dvr_command(rpc);
};

void Dvr::shutdown() {
	dvr_rpc rpc = {
		.command = dvr_rpc::RPC_SHUTDOWN
	};
	enqueue_dvr_command(rpc);
};

void Dvr::enqueue_dvr_command(dvr_rpc rpc) {
	{
		std::lock_guard<std::mutex> lock(mtx);
		dvrQueue.push(rpc);
	}
	cv.notify_one();
}

void *Dvr::__THREAD__(void *param) {
	pthread_setname_np(pthread_self(), "__DVR");
	((Dvr *)param)->loop();
	return nullptr;
}

void Dvr::loop() {
	while (true) {
		std::unique_lock<std::mutex> lock(mtx);
		cv.wait(lock, [this] { return !this->dvrQueue.empty(); });
		if (dvrQueue.empty()) {
			break;
		}
		if (!dvrQueue.empty()) {
			dvr_rpc rpc = dvrQueue.front();
			dvrQueue.pop();
			lock.unlock();
			switch (rpc.command) {
			case dvr_rpc::RPC_SET_PARAMS:
				{
					SPDLOG_DEBUG("got rpc SET_PARAMS");
					if (dvr_file == NULL) {
						break;
					}
					init();
					break;
				}
			case dvr_rpc::RPC_START:
				{
					SPDLOG_DEBUG("got rpc START");
					if (dvr_file != NULL) {
						break;
					}
					start();
					if (video_frm_width > 0 && video_frm_height > 0) {
						init();
					}
					break;
				}
			case dvr_rpc::RPC_STOP:
				{
					SPDLOG_DEBUG("got rpc STOP");
					if (dvr_file == NULL) {
						break;
					}
					stop();
					break;
				}
			case dvr_rpc::RPC_TOGGLE:
				{
					SPDLOG_DEBUG("got rpc TOGGLE");
					if (dvr_file == NULL) {
						start();
						if (video_frm_width > 0 && video_frm_height > 0) {
							init();
						}
					} else {
						stop();
					}
					break;
				}
			case dvr_rpc::RPC_FRAME:
				{
					if (!_ready_to_write) {
						break;
					}
					std::shared_ptr<std::vector<uint8_t>> frame = rpc.frame;
					auto res = mp4_h26x_write_nal(mp4wr, frame->data(), frame->size(), 90000/video_framerate);
					if (!(MP4E_STATUS_OK == res || MP4E_STATUS_BAD_ARGUMENTS == res)) {
						spdlog::warn("mp4_h26x_write_nal failed with error {}", res);
					}
					break;
				}
			case dvr_rpc::RPC_SHUTDOWN:
				goto end;
			}
		}
	}
end:
	if (dvr_file != NULL) {
		stop();
	}
	spdlog::info("DVR thread done.");
}

int Dvr::start() {
	char *fname_tpl = filename_template;
	std::string rec_dir, filename_pattern;
	fs::path pathObj(filename_template);
	rec_dir = pathObj.parent_path().string();
	filename_pattern = pathObj.filename().string();
	std::string paddedNumber = "";

	// Ensure the directory exists
	if (!fs::exists(rec_dir))
	{
		spdlog::error("Error: Directory does not exist: {}", rec_dir);
		return -1;
	}

	if (dvr_filenames_with_sequence) {
		// Get the next file number
		std::regex pattern(R"(^(\d+)_.*)"); // Matches filenames that start with digits followed by '_'
		int maxNumber = -1;
		int nextFileNumber = 0;

		for (const auto &entry : fs::directory_iterator(rec_dir)) {
			if (entry.is_regular_file())
			{
				std::string filename = entry.path().filename().string();
				std::smatch match;

				if (std::regex_match(filename, match, pattern))
				{
					int number = std::stoi(match[1].str());
					maxNumber = std::max(maxNumber, number);
				}
			}
		}
		if (maxNumber == -1) {
			nextFileNumber = 0;
		} else {
			nextFileNumber = maxNumber + 1;
		}

		// Zero-pad the number
		std::ostringstream stream;
		stream << std::setw(SEQUENCE_PADDING) << std::setfill('0') << nextFileNumber;
		paddedNumber = stream.str() + "_";
	}

	// Generate timestamped filename
	std::time_t now = std::time(nullptr);
	std::tm *localTime = std::localtime(&now);

	char formattedFilename[256];
	std::strftime(formattedFilename, sizeof(formattedFilename), filename_pattern.c_str(), localTime);

	// Construct final filename
	std::string finalFilename = rec_dir + "/" + paddedNumber + formattedFilename;

	if ((dvr_file = fopen(finalFilename.c_str(), "w")) == NULL) {
		spdlog::error("unable to open DVR file {}", finalFilename);
		return -1;
	}
	osd_publish_bool_fact("dvr.recording", NULL, 0, true);
	dvr_enabled = 1;
	mux = MP4E_open(0 /*sequential_mode*/, mp4_fragmentation_mode, dvr_file, write_callback);
	return 0;
}

void Dvr::init() {
	spdlog::info("setting up dvr and mux to {}x{}", video_frm_width, video_frm_height);
	if (MP4E_STATUS_OK != mp4_h26x_write_init(mp4wr, mux,
											  video_frm_width,
											  video_frm_height,
											  codec==VideoCodec::H265)) {
		spdlog::error("mp4_h26x_write_init failed");
		mux = NULL;
		dvr_file = NULL;
	}
	_ready_to_write = 1;
}

void Dvr::stop() {
	MP4E_close(mux);
	mp4_h26x_write_close(mp4wr);
	fclose(dvr_file);
	dvr_file = NULL;
	osd_publish_bool_fact("dvr.recording", NULL, 0, false);
	dvr_enabled = 0;
	_ready_to_write = 0;
}


// C-compatible interface
extern "C" {
	void dvr_start_recording(Dvr* dvr) {
		if (dvr) {
			dvr->start_recording();
		}
	}

	void dvr_stop_recording(Dvr* dvr) {
		if (dvr) {
			dvr->stop_recording();
		}
	}

	void dvr_set_video_framerate(Dvr* dvr, int f) {
		if (dvr) {
			dvr->set_video_framerate(f);
		}
	}
}
