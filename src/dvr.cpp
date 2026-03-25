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

// Strip H265 AUD (type 35), PREFIX_SEI (type 39), and SUFFIX_SEI (type 40)
// from an Annex-B bitstream before handing it to minimp4, which rejects these
// NAL types and would otherwise fail to write any frame.
static std::shared_ptr<std::vector<uint8_t>>
hevc_strip_supplemental(const uint8_t *data, size_t len)
{
    auto out = std::make_shared<std::vector<uint8_t>>();
    out->reserve(len);
    const uint8_t *p   = data;
    const uint8_t *end = data + len;
    for (;; p++) {
        int nal_size;
        p = find_nal_unit(p, (int)(end - p), &nal_size);
        if (!nal_size)
            break;
        int nal_type = (p[0] >> 1) & 0x3f;
        if (nal_type != 35 && nal_type != 39 && nal_type != 40) {
            static const uint8_t sc[] = {0, 0, 0, 1};
            out->insert(out->end(), sc, sc + 4);
            out->insert(out->end(), p, p + nal_size);
        }
    }
    return out->empty() ? nullptr : out;
}

int dvr_enabled = 0;
const int SEQUENCE_PADDING = 4; // Configurable padding for sequence numbers

int write_callback(int64_t offset, const void *buffer, size_t size, void *token){
	dvr_write_ctx *ctx = (dvr_write_ctx*)token;
	fseek(ctx->f, offset, SEEK_SET);
	int ret = fwrite(buffer, 1, size, ctx->f) != size;
	int64_t end = offset + (int64_t)size;
	if (end > ctx->file_size)
		ctx->file_size = end;
	return ret;
}

Dvr::Dvr(dvr_thread_params params) {
	filename_template = params.filename_template;
	mp4_fragmentation_mode = params.mp4_fragmentation_mode;
	dvr_filenames_with_sequence = params.dvr_filenames_with_sequence;
	video_framerate = params.video_framerate;
	max_file_size = params.max_file_size;
	video_frm_width = params.video_p.video_frm_width;
	video_frm_height = params.video_p.video_frm_height;
	codec = params.video_p.codec;
	write_ctx.f = NULL;
	write_ctx.file_size = 0;
	mp4wr = nullptr;
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
						   VideoCodec new_codec) {
	video_frm_width = video_frm_w;
	video_frm_height = video_frm_h;
	codec = new_codec;
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

void Dvr::set_max_file_size(int64_t size) {
	max_file_size = size;
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
					if (write_ctx.f == NULL) {
						break;
					}
					init();
					break;
				}
			case dvr_rpc::RPC_START:
				{
					SPDLOG_DEBUG("got rpc START");
					if (write_ctx.f != NULL) {
						break;
					}
					if (start() == 0) {
						idr_request_record_start();
						if (video_frm_width > 0 && video_frm_height > 0) {
							init();
						}
					}
					break;
				}
			case dvr_rpc::RPC_STOP:
				{
					SPDLOG_DEBUG("got rpc STOP");
					if (write_ctx.f == NULL) {
						break;
					}
					stop();
					break;
				}
			case dvr_rpc::RPC_TOGGLE:
				{
					SPDLOG_DEBUG("got rpc TOGGLE");
					if (write_ctx.f == NULL) {
						if (start() == 0) {
							idr_request_record_start();
							if (video_frm_width > 0 && video_frm_height > 0) {
								init();
							}
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
					if (codec == VideoCodec::H265) {
						frame = hevc_strip_supplemental(frame->data(), frame->size());
						if (!frame) break;
					}
					// Cache parameter sets as they arrive (they don't change)
					if (!params_complete)
						cache_parameter_sets(frame->data(), frame->size());
					// File splitting: split on next IDR when over size limit
					if (max_file_size > 0 && write_ctx.file_size > max_file_size) {
						if (!split_pending) {
							split_pending = true;
							if (on_start_cb) on_start_cb();
							idr_request_record_start();
						}
						if (params_complete && is_idr(frame->data(), frame->size())) {
							split();
							split_pending = false;
							// Build and replay VPS/SPS/PPS into new writer
							{
								static const uint8_t sc[] = {0, 0, 0, 1};
								std::vector<uint8_t> params;
								if (!cached_vps.empty()) {
									params.insert(params.end(), sc, sc + 4);
									params.insert(params.end(), cached_vps.begin(), cached_vps.end());
								}
								if (!cached_sps.empty()) {
									params.insert(params.end(), sc, sc + 4);
									params.insert(params.end(), cached_sps.begin(), cached_sps.end());
								}
								if (!cached_pps.empty()) {
									params.insert(params.end(), sc, sc + 4);
									params.insert(params.end(), cached_pps.begin(), cached_pps.end());
								}
								mp4_h26x_write_nal(mp4wr, params.data(), params.size(), 0);
							}
							mp4_h26x_write_nal(mp4wr, frame->data(),
								frame->size(), 90000/video_framerate);
							break;
						}
					}
					mp4_h26x_write_nal(mp4wr, frame->data(), frame->size(), 90000/video_framerate);
					break;
				}
			case dvr_rpc::RPC_SHUTDOWN:
				goto end;
			}
		}
	}
end:
	if (write_ctx.f != NULL) {
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

	// Store base path (without extension) for split naming
	split_part = 0;
	if (finalFilename.size() >= 4 && finalFilename.substr(finalFilename.size()-4) == ".mp4") {
		current_base_path = finalFilename.substr(0, finalFilename.size()-4);
	} else {
		current_base_path = finalFilename;
	}

	if ((write_ctx.f = fopen(finalFilename.c_str(), "w")) == NULL) {
		spdlog::error("unable to open DVR file {}", finalFilename);
		return -1;
	}
	write_ctx.file_size = 0;
	mux = MP4E_open(0 /*sequential_mode*/, mp4_fragmentation_mode, &write_ctx, write_callback);
	if (max_file_size > 0)
		spdlog::info("DVR file splitting enabled at {} MB", max_file_size / (1024*1024));
	return 0;
}

void Dvr::init() {
	spdlog::info("setting up dvr and mux to {}x{}", video_frm_width, video_frm_height);
	if (!mp4wr)
		mp4wr = (mp4_h26x_writer_t *)malloc(sizeof(mp4_h26x_writer_t));
	if (MP4E_STATUS_OK != mp4_h26x_write_init(mp4wr, mux,
											  video_frm_width,
											  video_frm_height,
											  codec==VideoCodec::H265)) {
		spdlog::error("mp4_h26x_write_init failed");
		mux = NULL;
		write_ctx.f = NULL;
	}
	_ready_to_write = 1;
	if (on_start_cb) on_start_cb();
}

void Dvr::stop() {
	MP4E_close(mux);
	mux = nullptr;
	if (mp4wr) {
		mp4_h26x_write_close(mp4wr);  // frees the struct (minimp4 API)
		mp4wr = nullptr;
	}
	fclose(write_ctx.f);
	write_ctx.f = NULL;
	write_ctx.file_size = 0;
	_ready_to_write = 0;
}

// Check if buffer contains an IDR/IRAP slice.
bool Dvr::is_idr(const uint8_t *data, size_t len) {
	const uint8_t *p = data;
	const uint8_t *end = data + len;

	for (;; p++) {
		int nal_size;
		p = find_nal_unit(p, (int)(end - p), &nal_size);
		if (!nal_size) break;

		if (codec == VideoCodec::H265) {
			int nal_type = (p[0] >> 1) & 0x3f;
			if (nal_type >= 16 && nal_type <= 23) return true; // IRAP
		} else {
			int nal_type = p[0] & 0x1f;
			if (nal_type == 5) return true; // IDR
		}
	}
	return false;
}

// Cache VPS/SPS/PPS NALs individually from any buffer.
// Called on each frame until all required params are collected.
void Dvr::cache_parameter_sets(const uint8_t *data, size_t len) {
	const uint8_t *p = data;
	const uint8_t *end = data + len;

	for (;; p++) {
		int nal_size;
		p = find_nal_unit(p, (int)(end - p), &nal_size);
		if (!nal_size) break;

		if (codec == VideoCodec::H265) {
			int t = (p[0] >> 1) & 0x3f;
			if (t == 32 && cached_vps.empty())
				cached_vps.assign(p, p + nal_size);
			else if (t == 33 && cached_sps.empty())
				cached_sps.assign(p, p + nal_size);
			else if (t == 34 && cached_pps.empty())
				cached_pps.assign(p, p + nal_size);
		} else {
			int t = p[0] & 0x1f;
			if (t == 7 && cached_sps.empty())
				cached_sps.assign(p, p + nal_size);
			else if (t == 8 && cached_pps.empty())
				cached_pps.assign(p, p + nal_size);
		}
	}

	if (codec == VideoCodec::H265)
		params_complete = !cached_vps.empty() && !cached_sps.empty() && !cached_pps.empty();
	else
		params_complete = !cached_sps.empty() && !cached_pps.empty();
}

void Dvr::split() {
	spdlog::info("DVR file split at {} MB (part {})",
		write_ctx.file_size / (1024*1024), split_part + 1);

	// Close current file
	MP4E_close(mux);
	mux = nullptr;
	mp4_h26x_write_close(mp4wr);
	mp4wr = nullptr;
	fclose(write_ctx.f);
	write_ctx.f = NULL;
	write_ctx.file_size = 0;

	// Open next part
	split_part++;
	std::string nextFilename = current_base_path + "_part" + std::to_string(split_part + 1) + ".mp4";
	if ((write_ctx.f = fopen(nextFilename.c_str(), "w")) == NULL) {
		spdlog::error("unable to open DVR split file {}", nextFilename);
		_ready_to_write = 0;
		return;
	}
	mux = MP4E_open(0, mp4_fragmentation_mode, &write_ctx, write_callback);
	mp4wr = (mp4_h26x_writer_t *)malloc(sizeof(mp4_h26x_writer_t));
	if (MP4E_STATUS_OK != mp4_h26x_write_init(mp4wr, mux,
		video_frm_width, video_frm_height, codec == VideoCodec::H265)) {
		spdlog::error("mp4_h26x_write_init failed on split file");
		_ready_to_write = 0;
	}
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
