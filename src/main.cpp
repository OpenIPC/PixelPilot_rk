#define MODULE_TAG "pixelpilot"

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
#include <nlohmann/json.hpp>
#include "spdlog/spdlog.h"

extern "C" {
#include "main.h"
#include "drm.h"

#include "mavlink/common/mavlink.h"
#include "mavlink.h"
#include "input.h"
}

#include "osd.h"
#include "osd.hpp"
#include "wfbcli.hpp"
#include "dvr.h"
#include "gstrtpreceiver.h"
#include "scheduling_helper.hpp"
#include "time_util.h"
#include "pixelpilot_config.h"


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
extern bool osd_update_ready;
extern bool gsmenu_enabled;
int video_zpos = 1;

bool mavlink_dvr_on_arm = false;
bool osd_custom_message = false;
bool disable_vsync = false;
uint32_t refresh_frequency_ms = 1000;

VideoCodec codec = VideoCodec::H265;
Dvr *dvr = NULL;

void init_buffer(MppFrame frame) {
	output_list->video_frm_width = mpp_frame_get_width(frame);
	output_list->video_frm_height = mpp_frame_get_height(frame);
	RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
	RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
	MppFrameFormat fmt = mpp_frame_get_fmt(frame);
	assert((fmt == MPP_FMT_YUV420SP) || (fmt == MPP_FMT_YUV420SP_10BIT));

	spdlog::info("Frame info changed {}({})x{}({})",
				 output_list->video_frm_width, hor_stride, output_list->video_frm_height, ver_stride);

	output_list->video_fb_x = 0;
	output_list->video_fb_y = 0;
	output_list->video_fb_width = output_list->mode.hdisplay;
	output_list->video_fb_height =output_list->mode.vdisplay;	

	osd_publish_uint_fact("video.width", NULL, 0, output_list->video_frm_width);
	osd_publish_uint_fact("video.height", NULL, 0, output_list->video_frm_height);

	if (mpi.frm_grp) {
		spdlog::debug("Freeing current mpp_buffer_group");
		
		// First clean up all DRM resources for existing frames
		for (int i = 0; i < MAX_FRAMES; i++) {
			if (mpi.frame_to_drm[i].fb_id) {
				drmModeRmFB(drm_fd, mpi.frame_to_drm[i].fb_id);
				mpi.frame_to_drm[i].fb_id = 0;
			}
			if (mpi.frame_to_drm[i].prime_fd >= 0) {
				close(mpi.frame_to_drm[i].prime_fd);
				mpi.frame_to_drm[i].prime_fd = -1;
			}
			if (mpi.frame_to_drm[i].handle) {
				struct drm_mode_destroy_dumb dmd = {
					.handle = mpi.frame_to_drm[i].handle,
				};
				ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmd);
				mpi.frame_to_drm[i].handle = 0;
			}
		}
		
		mpp_buffer_group_clear(mpi.frm_grp);
		mpp_buffer_group_put(mpi.frm_grp);  // This is important to release the group
		mpi.frm_grp = NULL;
	}

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

	ret = modeset_perform_modeset(drm_fd, output_list, output_list->video_request, &output_list->video_plane, mpi.frame_to_drm[0].fb_id, output_list->video_frm_width, output_list->video_frm_height, video_zpos);
	assert(ret >= 0);

	// dvr setup
	if (dvr != NULL){
		dvr->set_video_params(output_list->video_frm_width, output_list->video_frm_height, codec);
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
	pthread_setname_np(pthread_self(), "__FRAME");

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
	spdlog::info("Frame thread done.");
	return nullptr;
}


void *__DISPLAY_THREAD__(void *param)
{
	int ret;	
	pthread_setname_np(pthread_self(), "__DISPLAY");

	while (!frm_eos) {
		int fb_id;
		bool osd_update;
		
		ret = pthread_mutex_lock(&video_mutex);
		assert(!ret);
		while (output_list->video_fb_id==0 && !osd_update_ready) {
			pthread_cond_wait(&video_cond, &video_mutex);
			assert(!ret);
			if (output_list->video_fb_id == 0 && frm_eos) {
				ret = pthread_mutex_unlock(&video_mutex);
				assert(!ret);
				goto end;
			}
		}
		fb_id = output_list->video_fb_id;
		osd_update = osd_update_ready;

        uint64_t decoding_pts=fb_id != 0 ? output_list->decoding_pts : get_time_ms();
		output_list->video_fb_id=0;
		osd_update_ready = false;
		ret = pthread_mutex_unlock(&video_mutex);
		assert(!ret);

		// create new video_request
		drmModeAtomicFree(output_list->video_request);
		output_list->video_request = drmModeAtomicAlloc();

		// show DRM FB in plane
		uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;
		if (fb_id != 0) {
			flags = disable_vsync ? DRM_MODE_ATOMIC_NONBLOCK : DRM_MODE_ATOMIC_ALLOW_MODESET;
			ret = set_drm_object_property(output_list->video_request, &output_list->video_plane, "FB_ID", fb_id);
			assert(ret>0);
		}

		if(enable_osd) {
			ret = pthread_mutex_lock(&osd_mutex);
			assert(!ret);		
			ret = set_drm_object_property(output_list->video_request, &output_list->osd_plane, "FB_ID", output_list->osd_bufs[output_list->osd_buf_switch].fb);
			assert(ret>0);
		}
		drmModeAtomicCommit(drm_fd, output_list->video_request, flags, NULL);
		ret = pthread_mutex_unlock(&osd_mutex);
		assert(!ret);
		osd_publish_uint_fact("video.displayed_frame", NULL, 0, 1);
		uint64_t decode_and_handover_display_ms=get_time_ms()-decoding_pts;
		osd_publish_uint_fact("video.decode_and_handover_ms", NULL, 0, decode_and_handover_display_ms);
	}
end:	
	spdlog::info("Display thread done.");
	return nullptr;
}

// signal

int signal_flag = 0;
int return_value = 0;

void sig_handler(int signum)
{
	spdlog::info("Received signal {}", signum);
	signal_flag++;
	mavlink_thread_signal++;
	wfb_thread_signal++;
	osd_thread_signal++;
	if (dvr != NULL) {
		dvr->shutdown();
	}
	return_value = signum;
}

void sigusr1_handler(int signum) {
	spdlog::info("Received signal {}", signum);
	if (dvr) {
		dvr->toggle_recording();
	}
}

void sigusr2_handler(int signum) {
    // Toggle the disable_vsync flag
    disable_vsync = disable_vsync ^ 1;

    // Open the file for writing
    std::ofstream outFile("/run/pixelpilot.msg");
    if (!outFile.is_open()) {
        spdlog::error("Error opening file!");
        return; // Exit the function if the file cannot be opened
    }

    // Write the formatted text to the file
    outFile << "disable_vsync: " << std::boolalpha << disable_vsync << std::endl;
    outFile.close();

    // Log the new state of disable_vsync
    spdlog::info("disable_vsync: {}", disable_vsync);
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
        osd_publish_uint_fact("video.decoder_feed_time_ms", NULL, 0, elapsed);
        if (elapsed > 100) {
            decoder_stalled_count++;
            spdlog::warn("Cannot feed decoder, stalled {} ?", decoder_stalled_count);
            return false;
        }
        usleep(2 * 1000);
    }
    return true;
}

uint64_t first_frame_ms=0;
void read_gstreamerpipe_stream(MppPacket *packet, int gst_udp_port, const char *sock ,const VideoCodec& codec){
	std::unique_ptr<GstRtpReceiver> receiver;
	if (sock) {
		receiver = std::make_unique<GstRtpReceiver>(sock, codec);
	} else {
		receiver = std::make_unique<GstRtpReceiver>(gst_udp_port, codec);
	}
	long long bytes_received = 0; 
	uint64_t period_start=0;
    auto cb=[&packet,/*&decoder_stalled_count,*/ &bytes_received, &period_start](std::shared_ptr<std::vector<uint8_t>> frame){
        // Let the gst pull thread run at quite high priority
        static bool first= false;
        if(first){
            SchedulingHelper::set_thread_params_max_realtime("DisplayThread",SchedulingHelper::PRIORITY_REALTIME_LOW);
            first= false;
        }
		bytes_received += frame->size();
		uint64_t now = get_time_ms();
		osd_publish_uint_fact("gstreamer.received_bytes", NULL, 0, frame->size());
        feed_packet_to_decoder(packet,frame->data(),frame->size());
        if (dvr_enabled && dvr != NULL) {
			dvr->frame(frame);
        }
    };
    receiver->start_receiving(cb);
    while (!signal_flag){
        sleep(10);
    }
    receiver->stop_receiving();
    spdlog::info("Feeding eos");
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
        spdlog::warn("Could not set control {} {}", control, enable);
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
        spdlog::warn("{} failed to get decoder cfg ret {}", ctx, ret);
        assert(false);
    }
    // split_parse is to enable mpp internal frame spliter when the input
    // packet is not aplited into frames.
    RK_U32 need_split   = 1;
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    if (ret) {
        spdlog::warn("{} failed to set split_parse ret {}", ctx, ret);
        assert(false);
    }
    ret = mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
    if (ret) {
        spdlog::warn("{} failed to set cfg {} ret {}", ctx, cfg, ret);
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
    "\n\t\tPixelPilot FPV Decoder for Rockchip (%d.%d)\n"
    "\n"
    "  Usage:\n"
    "    pixelpilot [Arguments]\n"
    "\n"
    "  Arguments:\n"
    "    -p <port>              - UDP port for RTP video stream         (Default: 5600)\n"
    "\n"
    "    --socket <socket>      - read data from socket\n"
    "\n"
    "    --mavlink-port <port>  - UDP port for mavlink telemetry        (Default: 14550)\n"
    "\n"
    "    --mavlink-dvr-on-arm   - Start recording when armed\n"
    "\n"
    "    --codec <codec>        - Video codec, should be the same as on VTX  (Default: h265 <h264|h265>)\n"
    "\n"
    "    --log-level <level>    - Log verbosity level, debug|info|warn|error (Default: info)\n"
    "\n"
    "    --osd                  - Enable OSD\n"
    "\n"
    "    --osd-config <file>    - Path to OSD configuration file\n"
    "\n"
    "    --osd-refresh <rate>   - Defines the delay between osd refresh (Default: 1000 ms)\n"
    "\n"
    "    --osd-custom-message   - Enables the display of /run/pixelpilot.msg (beta feature, may be removed)\n"
    "\n"
    "    --disable-gsmenu       - Disables the gsmenu and frees up gpios\n"
    "\n"
    "    --dvr-template <path>  - Save the video feed (no osd) to the provided filename template.\n"
    "                             DVR is toggled by SIGUSR1 signal\n"
    "                             Supports placeholders %%Y - year, %%m - month, %%d - day,\n"
    "                             %%H - hour, %%M - minute, %%S - second. Ex: /media/DVR/%%Y-%%m-%%d_%%H-%%M-%%S.mp4\n"
    "\n"
	"    --dvr-sequenced-files  - Prepend a sequence number to the names of the dvr files\n"
	"\n"
    "    --dvr-start            - Start DVR immediately\n"
    "\n"
    "    --dvr-framerate <rate> - Force the dvr framerate for smoother dvr, ex: 60\n"
    "\n"
    "    --dvr-fmp4             - Save the video feed as a fragmented mp4\n"
    "\n"
    "    --screen-mode <mode>   - Override default screen mode. <width>x<heigth>@<fps> ex: 1920x1080@120\n"
    "\n"
	"    --disable-vsync         - Disable VSYNC commits\n"
	"\n"
    "    --screen-mode-list     - Print the list of supported screen modes and exit.\n"
    "\n"
    "    --wfb-api-port         - Port of wfb-server for cli statistics. (Default: 8003)\n"
	"                             Use \"0\" to disable this stats\n"
    "\n"
    "    --version              - Show program version\n"
    "\n", APP_VERSION_MAJOR, APP_VERSION_MINOR
  );
}

// main

int main(int argc, char **argv)
{
	int ret;	
	int i, j;
	int mavlink_thread = 0;
	int dvr_autostart = 0;
	int print_modelist = 0;
	char* dvr_template = NULL;
	int video_framerate = -1;
	int mp4_fragmentation_mode = 0;
	bool dvr_filenames_with_sequence = false;
	uint16_t listen_port = 5600;
	const char* unix_socket = NULL;
	uint16_t wfb_port = 8003;
	uint16_t mode_width = 0;
	uint16_t mode_height = 0;
	uint32_t mode_vrefresh = 0;
	std::string osd_config_path;
	auto log_level = spdlog::level::info;
	
    std::string pidFilePath = "/run/pixelpilot.pid";
    std::ofstream pidFile(pidFilePath);
    pidFile << getpid();
    pidFile.close();

	// Load console arguments
	__BeginParseConsoleArguments__(printHelp) 
	
	__OnArgument("-p") {
		listen_port = atoi(__ArgValue);
		continue;
	}
	
	__OnArgument("--socket") {
		unix_socket = const_cast<char*>(__ArgValue);
		continue;
	}

	__OnArgument("--codec") {
		char * codec_str = const_cast<char*>(__ArgValue);
		codec = video_codec(codec_str);
		if (codec == VideoCodec::UNKNOWN ) {
			fprintf(stderr, "unsupported video codec");
			return -1;
		}
		continue;
	}

	__OnArgument("--dvr") {
		dvr_template = const_cast<char*>(__ArgValue);
		dvr_autostart = 1;
		fprintf(stderr, "--dvr is deprecated. Use --dvr-template and --dvr-start instead.\n");
		continue;
	}

	__OnArgument("--dvr-start") {
		dvr_autostart = 1;
		continue;
	}

	__OnArgument("--dvr-template") {
		dvr_template = const_cast<char*>(__ArgValue);
		continue;
	}

	__OnArgument("--dvr-sequenced-files") {
		dvr_filenames_with_sequence = true;
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

	__OnArgument("--log-level") {
		std::string log_l = std::string(__ArgValue);
		if (log_l == "info") {
			log_level = spdlog::level::info;
		} else if (log_l == "debug"){
			log_level = spdlog::level::debug;
			spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%s:%#] [%^%l%$] %v");
		} else if (log_l == "warn"){
			log_level = spdlog::level::warn;
		} else if (log_l == "error"){
			log_level = spdlog::level::err;
		} else {
			fprintf(stderr, "invalid log level %s\n", log_l.c_str());
			printHelp();
			return -1;
		}
		continue;
	}

	__OnArgument("--mavlink-port") {
		mavlink_port = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--mavlink-dvr-on-arm") {
		mavlink_dvr_on_arm = true;
		continue;
	}

	__OnArgument("--osd") {
		enable_osd = 1;
		mavlink_thread = 1;
		continue;
	}
	__OnArgument("--osd-config") {
		osd_config_path = std::string(__ArgValue);
		continue;
	}
	__OnArgument("--osd-refresh") {
		refresh_frequency_ms = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--osd-elements") {
		spdlog::warn("--osd-elements parameter is removed.");
		char* elements = const_cast<char*> (__ArgValue);
		continue;
	}

	__OnArgument("--osd-telem-lvl") {
		spdlog::warn("--osd-telem-lvl parameter is removed.");
		continue;
	}

	__OnArgument("--osd-custom-message") {
		osd_custom_message = true;
		continue;
	}

	__OnArgument("--disable-gsmenu") {
		gsmenu_enabled = false;
		continue;
	}	

	__OnArgument("--screen-mode") {
		char* mode = const_cast<char*>(__ArgValue);
		mode_width = atoi(strtok(mode, "x"));
		mode_height = atoi(strtok(NULL, "@"));
		mode_vrefresh = atoi(strtok(NULL, "@"));
		continue;
	}

	__OnArgument("--disable-vsync") {
		disable_vsync = true;
		continue;
	}

	__OnArgument("--screen-mode-list") {
		print_modelist = 1;
		continue;
	}

	__OnArgument("--wfb-api-port") {
		wfb_port = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--version") {
		printf("PixelPilot Rockchip %d.%d\n", APP_VERSION_MAJOR, APP_VERSION_MINOR);
		return 0;
	}

	__EndParseConsoleArguments__

	spdlog::set_level(log_level);

	if (dvr_template != NULL && video_framerate < 0 ) {
		printf("--dvr-framerate must be provided when dvr is enabled.\n");
		return 0;
	}

	printf("PixelPilot Rockchip %d.%d\n", APP_VERSION_MAJOR, APP_VERSION_MINOR);

	spdlog::info("disable_vsync: {}", disable_vsync);

	if (enable_osd == 0 ) {
		video_zpos = 4;
	}
	
	MppCodingType mpp_type = MPP_VIDEO_CodingHEVC;
	if(codec==VideoCodec::H264) {
		mpp_type = MPP_VIDEO_CodingAVC;
	}
	ret = mpp_check_support_format(MPP_CTX_DEC, mpp_type);
	assert(!ret);
	
	//////////////////////////////////  DRM SETUP
	ret = modeset_open(&drm_fd, "/dev/dri/card0");
	if (ret < 0) {
		spdlog::warn("modeset_open() =  {}", ret);
	}
	assert(drm_fd >= 0);
	if (print_modelist) {
		modeset_print_modes(drm_fd);
		close(drm_fd);
		return 0;
	}

	output_list = modeset_prepare(drm_fd, mode_width, mode_height, mode_vrefresh);
	if (!output_list) {
		fprintf(stderr,
				"cannot initialize display. Is display connected? Is --screen-mode correct?\n");
		return -2;
	}
	
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


	////////////////////////////////// SIGNAL SETUP

	signal(SIGINT, sig_handler);
	signal(SIGPIPE, sig_handler);
	if (dvr_template) {
		signal(SIGUSR1, sigusr1_handler);
	}
	signal(SIGUSR2, sigusr2_handler);
 	//////////////////// THREADS SETUP
	
	ret = pthread_mutex_init(&video_mutex, NULL);
	assert(!ret);
	ret = pthread_cond_init(&video_cond, NULL);
	assert(!ret);

	pthread_t tid_frame, tid_display, tid_osd, tid_mavlink, tid_dvr, tid_wfbcli;
	if (dvr_template != NULL) {
		dvr_thread_params args;
		args.filename_template = dvr_template;
		args.mp4_fragmentation_mode = mp4_fragmentation_mode;
		args.dvr_filenames_with_sequence = dvr_filenames_with_sequence;
		args.video_framerate = video_framerate;
		args.video_p.video_frm_width = output_list->video_frm_width;
		args.video_p.video_frm_height = output_list->video_frm_height;
		args.video_p.codec = codec;
		dvr = new Dvr(args);
		ret = pthread_create(&tid_dvr, NULL, &Dvr::__THREAD__, dvr);
		if (dvr_autostart) {
			dvr->start_recording();
		}
	}
	ret = pthread_create(&tid_frame, NULL, __FRAME_THREAD__, NULL);
	assert(!ret);
	ret = pthread_create(&tid_display, NULL, __DISPLAY_THREAD__, NULL);
	assert(!ret);
	if (enable_osd) {
		nlohmann::json osd_config;
		if(osd_config_path != "") {
			std::ifstream f(osd_config_path);
			osd_config = nlohmann::json::parse(f);
		} else {
			osd_config = {};
		}
		if (mavlink_thread) {
			ret = pthread_create(&tid_mavlink, NULL, __MAVLINK_THREAD__, &signal_flag);
			assert(!ret);
		}
		if (wfb_port) {
			wfb_thread_params *wfb_args = (wfb_thread_params *)malloc(sizeof *wfb_args);
			wfb_args->port = wfb_port;
			ret = pthread_create(&tid_wfbcli, NULL, __WFB_CLI_THREAD__, wfb_args);
			assert(!ret);
		}

		osd_thread_params *args = (osd_thread_params *)malloc(sizeof *args);
        args->fd = drm_fd;
        args->out = output_list;
		args->config = osd_config;
		ret = pthread_create(&tid_osd, NULL, __OSD_THREAD__, args);
		assert(!ret);
	}

	////////////////////////////////////////////// MAIN LOOP
    read_gstreamerpipe_stream((void**)packet, listen_port, unix_socket, codec);

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
		ret = pthread_join(tid_wfbcli, NULL);
		assert(!ret);
		ret = pthread_join(tid_osd, NULL);
		assert(!ret);
	}
	if (dvr_template != NULL ){
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

    remove(pidFilePath.c_str());

	restore_stdin();
	return return_value;
}
