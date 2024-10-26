#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "dvr.h"
#include "minimp4.h"

#include "gstrtpreceiver.h"
extern "C" {
#include "osd.h"
}

int dvr_enabled = 0;

int write_callback(int64_t offset, const void *buffer, size_t size, void *token){
	FILE *f = (FILE*)token;
	fseek(f, offset, SEEK_SET);
	return fwrite(buffer, 1, size, f) != size;
}

Dvr::Dvr(dvr_thread_params params) {
	filename_template = params.filename_template;
	mp4_fragmentation_mode = params.mp4_fragmentation_mode;
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
					printf("got rpc SET_PARAMS\n");
					if (dvr_file == NULL) {
						break;
					}
					init();
					break;
				}
			case dvr_rpc::RPC_START:
				{
					printf("got rpc START\n");
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
					printf("got rpc STOP\n");
					if (dvr_file == NULL) {
						break;
					}
					stop();
					break;
				}
			case dvr_rpc::RPC_TOGGLE:
				{
					printf("got rpc TOGGLE\n");
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
						printf("mp4_h26x_write_nal failed with error %d\n", res);
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
	printf("DVR thread done.\n");
}

int Dvr::start() {
	char *fname_tpl = filename_template;
	char fname[255];
	time_t t = time(NULL);
	strftime(fname, sizeof(fname), fname_tpl, localtime(&t));
	if ((dvr_file = fopen(fname,"w")) == NULL){
		fprintf(stderr, "ERROR: unable to open %s\n", fname);
		return -1;
	}
	osd_vars.enable_recording = 1;
	dvr_enabled = 1;
	mux = MP4E_open(0 /*sequential_mode*/, mp4_fragmentation_mode, dvr_file, write_callback);
	return 0;
}

void Dvr::init() {
	printf("setting up dvr and mux to %dx%d\n", video_frm_width, video_frm_height);
	if (MP4E_STATUS_OK != mp4_h26x_write_init(mp4wr, mux,
											  video_frm_width,
											  video_frm_height,
											  codec==VideoCodec::H265)) {
		fprintf(stderr, "error: mp4_h26x_write_init failed\n");
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
	osd_vars.enable_recording = 0;
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
}
