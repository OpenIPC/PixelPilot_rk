#ifndef DVR_H
#define DVR_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

#include "gstrtpreceiver.h"

enum DvrMode { DVR_MODE_RAW = 0, DVR_MODE_REENCODE = 1, DVR_MODE_BOTH = 2 };

struct MP4E_mux_tag;
struct mp4_h26x_writer_tag;

struct dvr_write_ctx {
    FILE *f;
    int64_t file_size;  // tracks max written offset
};

struct video_params {
    uint32_t video_frm_width;
    uint32_t video_frm_height;
    VideoCodec codec;
};

struct dvr_thread_params {
    char *filename_template;
    int mp4_fragmentation_mode = 0;
    bool dvr_filenames_with_sequence = false;
    int video_framerate = -1;
    int64_t max_file_size = 0;  // 0 = no limit; bytes
    video_params video_p;
};

struct dvr_rpc {
    enum {
        RPC_FRAME,
        RPC_STOP,
        RPC_START,
        RPC_TOGGLE,
        RPC_SHUTDOWN,
        RPC_SET_PARAMS
    } command;
    /* union { */
        std::shared_ptr<std::vector<uint8_t>> frame;
    /*     video_params params; */
    /* }; */
};


extern int dvr_enabled;

class Dvr {
public:
    explicit Dvr(dvr_thread_params params);
    virtual ~Dvr();

    void frame(std::shared_ptr<std::vector<uint8_t>> frame);
    void set_video_params(uint32_t video_frm_width,
                          uint32_t video_frm_height,
                          VideoCodec codec);
    void start_recording();
    void stop_recording();
    void set_video_framerate(int rate);
    void set_max_file_size(int64_t size);
    void toggle_recording();
    void shutdown();

    static void *__THREAD__(void *context);

    std::function<void()> on_start_cb;
private:
    /* void *start(); */
    /* void *stop(); */
    void enqueue_dvr_command(dvr_rpc rpc);

    void loop();
    int start();
    void stop();
    void init();
    void split();
    bool is_idr(const uint8_t *data, size_t len);
    void cache_parameter_sets(const uint8_t *data, size_t len);
private:
    std::queue<dvr_rpc> dvrQueue;
    std::mutex mtx;
    std::condition_variable cv;
    char *filename_template;
    int mp4_fragmentation_mode = 0;
    bool dvr_filenames_with_sequence = false;
    int video_framerate = -1;
    int64_t max_file_size = 0;
    uint32_t video_frm_width;
    uint32_t video_frm_height;
    VideoCodec codec;
    int _ready_to_write = 0;
    bool split_pending = false;
    int split_part = 0;
    std::string current_base_path;  // base path without .mp4 for split naming
    std::vector<uint8_t> cached_vps;   // H.265 only
    std::vector<uint8_t> cached_sps;
    std::vector<uint8_t> cached_pps;
    bool params_complete = false;

    dvr_write_ctx write_ctx;
    MP4E_mux_tag *mux;
    mp4_h26x_writer_tag *mp4wr;
};

#endif
