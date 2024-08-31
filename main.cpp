#define MODULE_TAG "fpvue"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>
#include <signal.h>
#include <fstream>
#include <queue>
#include <mutex>
#include <condition_variable>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <linux/videodev2.h>
#include <rockchip/rk_mpi.h>

extern "C" {
#include "main.h"
#include "drm.h"
#include "osd.h"
#include "rtp.h"

#include "mavlink/common/mavlink.h"
#include "mavlink.h"
}

#include "minimp4.h"
#include "gstrtpreceiver.h"
#include "scheduling_helper.hpp"
#include "time_util.h"
#include "fpvue_config.h"


#define READ_BUF_SIZE (1024*1024) // SZ_1M https://github.com/rockchip-linux/mpp/blob/ed377c99a733e2cdbcc457a6aa3f0fcd438a9dff/osal/inc/mpp_common.h#L179
#define MAX_FRAMES 24		// min 16 and 20+ recommended (mpp/readme.txt)

#define CODEC_ALIGN(x, a)   (((x)+(a)-1)&~((a)-1))

struct {
	MppCtx		  ctx;
	MppApi		  *mpi;
	
	struct timespec first_frame_ts;

	MppBufferGroup	frm_grp;
	struct {
		int prime_fd;
		uint32_t fb_id;
		uint32_t handle;
	} frame_to_drm[MAX_FRAMES];
} mpi;

struct timespec frame_stats[1000];

struct modeset_output *output_list;
int frm_eos = 0;
int drm_fd = 0;
pthread_mutex_t video_mutex;
pthread_cond_t video_cond;

int video_zpos = 1;
int video_framerate = -1;
int mp4_fragmentation_mode = 0;

VideoCodec codec = VideoCodec::H265;
FILE *dvr_file = NULL;
MP4E_mux_t *mux ;
mp4_h26x_writer_t mp4wr;

int write_callback(int64_t offset, const void *buffer, size_t size, void *token){
    FILE *f = (FILE*)token;
    fseek(f, offset, SEEK_SET);
    return fwrite(buffer, 1, size, f) != size;
}

void init_buffer(MppFrame frame) {
	output_list->video_frm_width = mpp_frame_get_width(frame);
	output_list->video_frm_height = mpp_frame_get_height(frame);
	RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
	RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
	MppFrameFormat fmt = mpp_frame_get_fmt(frame);
	assert((fmt == MPP_FMT_YUV420SP) || (fmt == MPP_FMT_YUV420SP_10BIT));

	printf("Frame info changed %d(%d)x%d(%d)\n", output_list->video_frm_width, hor_stride, output_list->video_frm_height, ver_stride);

	output_list->video_fb_x = 0;
	output_list->video_fb_y = 0;
	output_list->video_fb_width = output_list->mode.hdisplay;
	output_list->video_fb_height =output_list->mode.vdisplay;	

	osd_vars.video_width = output_list->video_frm_width;
	osd_vars.video_height = output_list->video_frm_height;

	// create new external frame group and allocate (commit flow) new DRM buffers and DRM FB
	int ret = mpp_buffer_group_get_external(&mpi.frm_grp, MPP_BUFFER_TYPE_DRM);
	assert(!ret);			

	for (int i=0; i<MAX_FRAMES; i++) {
		
		// new DRM buffer
		struct drm_mode_create_dumb dmcd;
		memset(&dmcd, 0, sizeof(dmcd));
		dmcd.bpp = fmt==MPP_FMT_YUV420SP?8:10;
		dmcd.width = hor_stride;
		dmcd.height = ver_stride*2; // documentation say not v*2/3 but v*2 (additional info included)
		do {
			ret = ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd);
		} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
		assert(!ret);
		// assert(dmcd.pitch==(fmt==MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8));
		// assert(dmcd.size==(fmt == MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8)*ver_stride*2);
		mpi.frame_to_drm[i].handle = dmcd.handle;
		
		// commit DRM buffer to frame group
		struct drm_prime_handle dph;
		memset(&dph, 0, sizeof(struct drm_prime_handle));
		dph.handle = dmcd.handle;
		dph.fd = -1;
		do {
			ret = ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
		} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
		assert(!ret);
		MppBufferInfo info;
		memset(&info, 0, sizeof(info));
		info.type = MPP_BUFFER_TYPE_DRM;
		info.size = dmcd.width*dmcd.height;
		info.fd = dph.fd;
		ret = mpp_buffer_commit(mpi.frm_grp, &info);
		assert(!ret);
		mpi.frame_to_drm[i].prime_fd = info.fd; // dups fd						
		if (dph.fd != info.fd) {
			ret = close(dph.fd);
			assert(!ret);
		}

		// allocate DRM FB from DRM buffer
		uint32_t handles[4], pitches[4], offsets[4];
		memset(handles, 0, sizeof(handles));
		memset(pitches, 0, sizeof(pitches));
		memset(offsets, 0, sizeof(offsets));
		handles[0] = mpi.frame_to_drm[i].handle;
		offsets[0] = 0;
		pitches[0] = hor_stride;						
		handles[1] = mpi.frame_to_drm[i].handle;
		offsets[1] = pitches[0] * ver_stride;
		pitches[1] = pitches[0];
		ret = drmModeAddFB2(drm_fd, output_list->video_frm_width, output_list->video_frm_height, DRM_FORMAT_NV12, handles, pitches, offsets, &mpi.frame_to_drm[i].fb_id, 0);
		assert(!ret);
	}

	// register external frame group
	ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_EXT_BUF_GROUP, mpi.frm_grp);
	ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

	ret = modeset_perform_modeset(drm_fd, output_list, output_list->video_request, &output_list->video_plane, mpi.frame_to_drm[0].fb_id, osd_vars.video_width, osd_vars.video_height, video_zpos);
	assert(ret >= 0);

	// dvr setup
	if (dvr_file != NULL){
		printf("setting up dvr and mux\n");
		mux = MP4E_open(0 /*sequential_mode*/, mp4_fragmentation_mode, dvr_file, write_callback);
		if (MP4E_STATUS_OK != mp4_h26x_write_init(&mp4wr, mux, output_list->video_frm_width, output_list->video_frm_height, codec==VideoCodec::H265))
		{
			printf("error: mp4_h26x_write_init failed\n");
			mux = NULL;
			dvr_file = NULL;
		}
	}
}

// __FRAME_THREAD__
//
// - allocate DRM buffers and DRM FB based on frame size
// - pick frame in blocking mode and output to screen overlay

void *__FRAME_THREAD__(void *param)
{
	SchedulingHelper::set_thread_params_max_realtime("FRAME_THREAD",SchedulingHelper::PRIORITY_REALTIME_MID);
	int i, ret;
	MppFrame  frame  = NULL;
	uint64_t last_frame_time;

	while (!frm_eos) {
		struct timespec ts, ats;
		
		assert(!frame);
		ret = mpi.mpi->decode_get_frame(mpi.ctx, &frame);
		assert(!ret);
		clock_gettime(CLOCK_MONOTONIC, &ats);
		if (frame) {
			if (mpp_frame_get_info_change(frame)) {
				// new resolution
				init_buffer(frame);
			} else {
				// regular frame received
				if (!mpi.first_frame_ts.tv_sec) {
					ts = ats;
					mpi.first_frame_ts = ats;
				}

				MppBuffer buffer = mpp_frame_get_buffer(frame);					
				if (buffer) {
					output_list->video_poc = mpp_frame_get_poc(frame);
					uint64_t feed_data_ts =  mpp_frame_get_pts(frame);

					MppBufferInfo info;
					ret = mpp_buffer_info_get(buffer, &info);
					assert(!ret);
					for (i=0; i<MAX_FRAMES; i++) {
						if (mpi.frame_to_drm[i].prime_fd == info.fd) break;
					}
					assert(i!=MAX_FRAMES);

					ts = ats;
					
					// send DRM FB to display thread
					ret = pthread_mutex_lock(&video_mutex);
					assert(!ret);
					output_list->video_fb_id = mpi.frame_to_drm[i].fb_id;
                    //output_list->video_fb_index=i;
                    output_list->decoding_pts=feed_data_ts;
					ret = pthread_cond_signal(&video_cond);
					assert(!ret);
					ret = pthread_mutex_unlock(&video_mutex);
					assert(!ret);
					
				}
			}
			
			frm_eos = mpp_frame_get_eos(frame);
			mpp_frame_deinit(&frame);
			frame = NULL;
		} else assert(0);
	}
	printf("Frame thread done.\n");
	return nullptr;
}


void *__DISPLAY_THREAD__(void *param)
{
	int ret;	
	int frame_counter = 0;
	float latency_avg[200];
	float min_latency = 1844674407370955161; // almost MAX_uint64_t
	float max_latency = 0;
    struct timespec fps_start, fps_end;
	clock_gettime(CLOCK_MONOTONIC, &fps_start);

	while (!frm_eos) {
		int fb_id;
		
		ret = pthread_mutex_lock(&video_mutex);
		assert(!ret);
		while (output_list->video_fb_id==0) {
			pthread_cond_wait(&video_cond, &video_mutex);
			assert(!ret);
			if (output_list->video_fb_id == 0 && frm_eos) {
				ret = pthread_mutex_unlock(&video_mutex);
				assert(!ret);
				goto end;
			}
		}
		struct timespec ts, ats;
		clock_gettime(CLOCK_MONOTONIC, &ats);
		fb_id = output_list->video_fb_id;

        uint64_t decoding_pts=output_list->decoding_pts;
		output_list->video_fb_id=0;
		ret = pthread_mutex_unlock(&video_mutex);
		assert(!ret);

		// show DRM FB in plane
		drmModeAtomicSetCursor(output_list->video_request, 0);
		ret = set_drm_object_property(output_list->video_request, &output_list->video_plane, "FB_ID", fb_id);
		assert(ret>0);

		ret = pthread_mutex_lock(&osd_mutex);
		assert(!ret);	
		ret = set_drm_object_property(output_list->video_request, &output_list->osd_plane, "FB_ID", output_list->osd_bufs[output_list->osd_buf_switch].fb);
		assert(ret>0);
		drmModeAtomicCommit(drm_fd, output_list->video_request, DRM_MODE_ATOMIC_NONBLOCK, NULL);
		ret = pthread_mutex_unlock(&osd_mutex);

		assert(!ret);
		frame_counter++;

		uint64_t decode_and_handover_display_ms=get_time_ms()-decoding_pts;
        //accumulate_and_print("D&Display",decode_and_handover_display_ms,&m_decode_and_handover_display_latency);
        
		clock_gettime(CLOCK_MONOTONIC, &fps_end);
		uint64_t time_us=(fps_end.tv_sec - fps_start.tv_sec)*1000000ll + ((fps_end.tv_nsec - fps_start.tv_nsec)/1000ll) % 1000000ll;
		if (time_us >= osd_vars.refresh_frequency_ms*1000) {
			float sum = 0;
			for (int i = 0; i < frame_counter; ++i) {
				sum += latency_avg[i];
				if (latency_avg[i] > max_latency) {
					max_latency = latency_avg[i];
				}
				if (latency_avg[i] < min_latency) {
					min_latency = latency_avg[i];
				}
			}
			osd_vars.latency_avg = sum / (frame_counter);
			osd_vars.latency_max = max_latency;
			osd_vars.latency_min = min_latency;
			osd_vars.current_framerate = frame_counter*(1000/osd_vars.refresh_frequency_ms);

			// printf("decoding decoding latency=%.2f ms (%.2f, %.2f), framerate=%d fps\n", osd_vars.latency_avg/1000.0, osd_vars.latency_max/1000.0, osd_vars.latency_min/1000.0, osd_vars.current_framerate);
			
			fps_start = fps_end;
			frame_counter = 0;
			max_latency = 0;
			min_latency = 1844674407370955161;
		}
		
		//struct timespec rtime = frame_stats[output_list->video_poc];
		latency_avg[frame_counter] = decode_and_handover_display_ms;
		//printf("decoding current_latency=%.2f ms\n",  latency_avg[frame_counter]/1000.0);
		
	}
end:	
	printf("Display thread done.\n");
	return nullptr;
}

// signal

int signal_flag = 0;

void sig_handler(int signum)
{
	printf("Received signal %d\n", signum);
	signal_flag++;
	mavlink_thread_signal++;
	osd_thread_signal++;
}

std::queue<std::shared_ptr<std::vector<uint8_t>>> dvrQueue;
std::mutex mtx;
std::condition_variable cv;

void *__DVR_THREAD__(void *param) {
	while (true) {
		std::unique_lock<std::mutex> lock(mtx);
		cv.wait(lock, [dvrQueue, signal_flag] { return !dvrQueue.empty() || signal_flag; });
		if (signal_flag && dvrQueue.empty()) {
			break;
		}
		if (!dvrQueue.empty()) {
			std::shared_ptr<std::vector<uint8_t>> frame = dvrQueue.front();
			dvrQueue.pop();
			lock.unlock();
			// Process the frame
			auto res = mp4_h26x_write_nal(&mp4wr, frame->data(), frame->size(), 90000/video_framerate);
			if (!(MP4E_STATUS_OK == res || MP4E_STATUS_BAD_ARGUMENTS == res)) {
				printf("mp4_h26x_write_nal failed with error %d\n", res);
			}
		}
	}
	MP4E_close(mux);
	mp4_h26x_write_close(&mp4wr);
	fclose(dvr_file);
	printf("DVR thread done.\n");
}

void enqueueDvrPacket(std::shared_ptr<std::vector<uint8_t>> frame) {
	{
		std::lock_guard<std::mutex> lock(mtx);
		dvrQueue.push(frame);
	}
	cv.notify_one();
}

int decoder_stalled_count=0;
bool feed_packet_to_decoder(MppPacket *packet,void* data_p,int data_len){
    mpp_packet_set_data(packet, data_p);
    mpp_packet_set_size(packet, data_len);
    mpp_packet_set_pos(packet, data_p);
    mpp_packet_set_length(packet, data_len);
    mpp_packet_set_pts(packet,(RK_S64) get_time_ms());
    // Feed the data to mpp until either timeout (in which case the decoder might have stalled)
    // or success
    uint64_t data_feed_begin = get_time_ms();
    int ret=0;
    while (!signal_flag && MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
        uint64_t elapsed = get_time_ms() - data_feed_begin;
        if (elapsed > 100) {
            decoder_stalled_count++;
            printf("Cannot feed decoder, stalled %d ?\n",decoder_stalled_count);
            return false;
        }
        usleep(2 * 1000);
    }
    return true;
}

uint64_t first_frame_ms=0;
void read_gstreamerpipe_stream(MppPacket *packet, int gst_udp_port, const VideoCodec& codec){
    GstRtpReceiver receiver(gst_udp_port, codec);
	long long bytes_received = 0; 
	uint64_t period_start=0;
    auto cb=[&packet,&decoder_stalled_count, &bytes_received, &period_start](std::shared_ptr<std::vector<uint8_t>> frame){
        // Let the gst pull thread run at quite high priority
        static bool first= false;
        if(first){
            SchedulingHelper::set_thread_params_max_realtime("DisplayThread",SchedulingHelper::PRIORITY_REALTIME_LOW);
            first= false;
        }
		bytes_received += frame->size();
		uint64_t now = get_time_ms();
		if ((now-period_start) >= 1000) {
			period_start = now;
			osd_vars.bw_curr = (osd_vars.bw_curr + 1) % 10;
			osd_vars.bw_stats[osd_vars.bw_curr] = bytes_received ;
			bytes_received = 0;
		}
        feed_packet_to_decoder(packet,frame->data(),frame->size());

		enqueueDvrPacket(frame);
    };
    receiver.start_receiving(cb);
    while (!signal_flag){
        sleep(10);
    }
    receiver.stop_receiving();
    printf("Feeding eos\n");
    mpp_packet_set_eos(packet);
    //mpp_packet_set_pos(packet, nal_buffer);
    mpp_packet_set_length(packet, 0);
    int ret=0;
    while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
        usleep(10000);
    }
};

void set_control_verbose(MppApi * mpi,  MppCtx ctx,MpiCmd control,RK_U32 enable){
    RK_U32 res = mpi->control(ctx, control, &enable);
    if(res){
        printf("Could not set control %d %d\n",control,enable);
        assert(false);
    }
}

void set_mpp_decoding_parameters(MppApi * mpi,  MppCtx ctx) {
    // config for runtime mode
    MppDecCfg cfg       = NULL;
    mpp_dec_cfg_init(&cfg);
    // get default config from decoder context
    int ret = mpi->control(ctx, MPP_DEC_GET_CFG, cfg);
    if (ret) {
        printf("%p failed to get decoder cfg ret %d\n", ctx, ret);
        assert(false);
    }
    // split_parse is to enable mpp internal frame spliter when the input
    // packet is not aplited into frames.
    RK_U32 need_split   = 1;
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    if (ret) {
        printf("%p failed to set split_parse ret %d\n", ctx, ret);
        assert(false);
    }
    ret = mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
    if (ret) {
        printf("%p failed to set cfg %p ret %d\n", ctx, cfg, ret);
        assert(false);
    }
	int mpp_split_mode =0;
    set_control_verbose(mpi,ctx,MPP_DEC_SET_PARSER_SPLIT_MODE, mpp_split_mode ? 0xffff : 0);
    set_control_verbose(mpi,ctx,MPP_DEC_SET_DISABLE_ERROR, 0xffff);
    set_control_verbose(mpi,ctx,MPP_DEC_SET_IMMEDIATE_OUT, 0xffff);
    set_control_verbose(mpi,ctx,MPP_DEC_SET_ENABLE_FAST_PLAY, 0xffff);
    //set_control_verbose(mpi,ctx,MPP_DEC_SET_ENABLE_DEINTERLACE, 0xffff);
    // Docu fast mode:
    // and improve the
    // parallelism of decoder hardware and software
    // we probably don't want that, since we don't need pipelining to hit our bitrate(s)
    int fast_mode = 0;
    set_control_verbose(mpi,ctx,MPP_DEC_SET_PARSER_FAST_MODE,fast_mode);
}

void printHelp() {
  printf(
    "\n\t\tFPVue FPV Decoder for Rockchip (%d.%d)\n"
    "\n"
    "  Usage:\n"
    "    fpvue [Arguments]\n"
    "\n"
    "  Arguments:\n"
    "    -p [Port]         		- Listen port                           (Default: 5600)\n"
    "\n"
    "    --osd          		- Enable OSD\n"
    "\n"
    "    --osd-elements 		- Customize osd elements   			    (Default: video,wfbng,telem)\n"
    "\n"
    "    --osd-telem-lvl		- Level of details for telemetry in the OSD (Default: 1 [1-2])\n"
    "\n"
    "    --osd-refresh  		- Defines the delay between osd refresh (Default: 1000 ms)\n"
    "\n"
    "    --dvr             		- Save the video feed (no osd) to the provided filename\n"
    "\n"
    "    --dvr-framerate        - Force the dvr framerate fro smoother dvr\n"
    "\n"
    "    --dvr-fmp4            	- Save the video feed as a fragmented mp4\n"
    "\n"
    "    --screen-mode   		- Override default screen mode. ex:1920x1080@120\n"
    "\n", fpvue_VERSION_MAJOR , fpvue_VERSION_MINOR
  );
}

// main

int main(int argc, char **argv)
{
	int ret;	
	int i, j;
	int enable_osd = 0;
	int mavlink_thread = 0;
	uint16_t listen_port = 5600;
	uint16_t mavlink_port = 14550;
	uint16_t mode_width = 0;
	uint16_t mode_height = 0;
	uint32_t mode_vrefresh = 0;
	
	osd_vars.enable_recording = 0;
	
	// Load console arguments
	__BeginParseConsoleArguments__(printHelp) 
	
	__OnArgument("-p") {
		listen_port = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--codec") {
		char * codec_str = const_cast<char*>(__ArgValue);
		codec = video_codec(codec_str);
		if (codec == VideoCodec::UNKNOWN ) {
			printf("unsupported video codec");
			return -1;
		}
		continue;
	}

	__OnArgument("--dvr") {
		if ((dvr_file = fopen(__ArgValue,"w")) == NULL){
			printf("ERROR: unable to open %s\n", dvr_file);	
			return -1;
		}

		osd_vars.enable_recording = 1;
		continue;
	}

	__OnArgument("--dvr-framerate") {
		video_framerate = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--dvr-fmp4") {
		mp4_fragmentation_mode = 1;
		continue;
	}

	__OnArgument("--mavlink-port") {
		mavlink_port = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--osd") {
		enable_osd = 1;
		osd_vars.plane_zpos = 2;
		osd_vars.enable_latency = 1;
		if (osd_vars.refresh_frequency_ms == 0 ){
			osd_vars.refresh_frequency_ms = 1000;
		} 
		osd_vars.enable_video = 1;
		osd_vars.enable_wfbng = 1;
		osd_vars.enable_telemetry = 1;
		mavlink_thread = 1;
		continue;
	}

	__OnArgument("--osd-refresh") {
		osd_vars.refresh_frequency_ms = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--osd-elements") {
		osd_vars.enable_video = 0;
		osd_vars.enable_wfbng = 0;
		osd_vars.enable_telemetry = 0;
		char* elements = const_cast<char*> (__ArgValue);
		char* element = strtok(elements, ",");
		while( element != NULL ) {
			if (!strcmp(element, "video")) {
				osd_vars.enable_video = 1;
			} else if (!strcmp(element, "wfbng")) {
				osd_vars.enable_wfbng = 1;
				mavlink_thread = 1;
			} else if (!strcmp(element, "telem")) {
				osd_vars.enable_telemetry = 1;
				mavlink_thread = 1;
			}
			element = strtok(NULL, ",");
		}
		continue;
	}

	__OnArgument("--osd-telem-lvl") {
		osd_vars.telemetry_level = atoi(__ArgValue);
		continue;
	}
	
	__OnArgument("--screen-mode") {
		char* mode = const_cast<char*>(__ArgValue);
		mode_width = atoi(strtok(mode, "x"));
		mode_height = atoi(strtok(NULL, "@"));
		mode_vrefresh = atoi(strtok(NULL, "@"));
		continue;
	}

	__OnArgument("--version") {
		printf("FPVue Rockchip %d.%d\n", fpvue_VERSION_MAJOR , fpvue_VERSION_MINOR);
		return 0;
	}

	__EndParseConsoleArguments__

	if (dvr_file != NULL && video_framerate < 0 ) {
		printf("--dvr-framerate must be provided when dvr is enabled.\n");
		return 0;
	}

	printf("FPVue Rockchip %d.%d\n", fpvue_VERSION_MAJOR , fpvue_VERSION_MINOR);

	if (enable_osd == 0 ) {
		video_zpos = 4;
	}
	
	MppCodingType mpp_type = MPP_VIDEO_CodingHEVC;
	if(codec==VideoCodec::H264) {
		mpp_type = MPP_VIDEO_CodingAVC;
	}
	ret = mpp_check_support_format(MPP_CTX_DEC, mpp_type);
	assert(!ret);

	////////////////////////////////// SIGNAL SETUP

	signal(SIGINT, sig_handler);
	signal(SIGPIPE, sig_handler);
	
	//////////////////////////////////  DRM SETUP
	ret = modeset_open(&drm_fd, "/dev/dri/card0");
	if (ret < 0) {
		printf("modeset_open() =  %d\n", ret);
	}
	assert(drm_fd >= 0);
	output_list = (struct modeset_output *)malloc(sizeof(struct modeset_output));
	ret = modeset_prepare(drm_fd, output_list, mode_width, mode_height, mode_vrefresh);
	assert(!ret);
	
	////////////////////////////////// MPI SETUP
	MppPacket packet;

	uint8_t* nal_buffer = (uint8_t*)malloc(1024 * 1024);
	assert(nal_buffer);
	ret = mpp_packet_init(&packet, nal_buffer, READ_BUF_SIZE);
	assert(!ret);

	ret = mpp_create(&mpi.ctx, &mpi.mpi);
	assert(!ret);
    set_mpp_decoding_parameters(mpi.mpi,mpi.ctx);
	ret = mpp_init(mpi.ctx, MPP_CTX_DEC, mpp_type);
    assert(!ret);
    set_mpp_decoding_parameters(mpi.mpi,mpi.ctx);

	// blocked/wait read of frame in thread
	int param = MPP_POLL_BLOCK;
	ret = mpi.mpi->control(mpi.ctx, MPP_SET_OUTPUT_BLOCK, &param);
	assert(!ret);

 	//////////////////// THREADS SETUP
	
	ret = pthread_mutex_init(&video_mutex, NULL);
	assert(!ret);
	ret = pthread_cond_init(&video_cond, NULL);
	assert(!ret);

	pthread_t tid_frame, tid_display, tid_osd, tid_mavlink, tid_dvr;
	if (dvr_file != NULL) {
		ret = pthread_create(&tid_dvr, NULL, __DVR_THREAD__, NULL);
	}
	ret = pthread_create(&tid_frame, NULL, __FRAME_THREAD__, NULL);
	assert(!ret);
	ret = pthread_create(&tid_display, NULL, __DISPLAY_THREAD__, NULL);
	assert(!ret);
	if (enable_osd) {
		if (mavlink_thread) {
			ret = pthread_create(&tid_mavlink, NULL, __MAVLINK_THREAD__, &signal_flag);
			assert(!ret);
		}
		osd_thread_params *args = (osd_thread_params *)malloc(sizeof *args);
        args->fd = drm_fd;
        args->out = output_list;
		ret = pthread_create(&tid_osd, NULL, __OSD_THREAD__, args);
		assert(!ret);
	}

	////////////////////////////////////////////// MAIN LOOP
    read_gstreamerpipe_stream((void**)packet, listen_port, codec);

	////////////////////////////////////////////// MPI CLEANUP

	ret = pthread_join(tid_frame, NULL);
	assert(!ret);
	
	ret = pthread_mutex_lock(&video_mutex);
	assert(!ret);	
	ret = pthread_cond_signal(&video_cond);
	assert(!ret);	
	ret = pthread_mutex_unlock(&video_mutex);
	assert(!ret);	

	ret = pthread_join(tid_display, NULL);
	assert(!ret);	
	
	ret = pthread_cond_destroy(&video_cond);
	assert(!ret);
	ret = pthread_mutex_destroy(&video_mutex);
	assert(!ret);

	if (mavlink_thread) {
		ret = pthread_join(tid_mavlink, NULL);
		assert(!ret);
	}
	if (enable_osd) {
		ret = pthread_join(tid_osd, NULL);
		assert(!ret);
	}
	if (dvr_file != NULL ){
		ret = pthread_join(tid_dvr, NULL);
		assert(!ret);
	}

	ret = mpi.mpi->reset(mpi.ctx);
	assert(!ret);

	if (mpi.frm_grp) {
		ret = mpp_buffer_group_put(mpi.frm_grp);
		assert(!ret);
		mpi.frm_grp = NULL;
		for (i=0; i<MAX_FRAMES; i++) {
			ret = drmModeRmFB(drm_fd, mpi.frame_to_drm[i].fb_id);
			assert(!ret);
			struct drm_mode_destroy_dumb dmdd;
			memset(&dmdd, 0, sizeof(dmdd));
			dmdd.handle = mpi.frame_to_drm[i].handle;
			do {
				ret = ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmdd);
			} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
			assert(!ret);
		}
	}
		
	mpp_packet_deinit(&packet);
	mpp_destroy(mpi.ctx);
	free(nal_buffer);
	
	////////////////////////////////////////////// DRM CLEANUP
	restore_planes_zpos(drm_fd, output_list);
	drmModeSetCrtc(drm_fd,
			       output_list->saved_crtc->crtc_id,
			       output_list->saved_crtc->buffer_id,
			       output_list->saved_crtc->x,
			       output_list->saved_crtc->y,
			       &output_list->connector.id,
			       1,
			       &output_list->saved_crtc->mode);
	drmModeFreeCrtc(output_list->saved_crtc);
	drmModeAtomicFree(output_list->video_request);
	drmModeAtomicFree(output_list->osd_request);
	modeset_cleanup(drm_fd, output_list);
	close(drm_fd);

	return 0;
}