//
// Created by https://github.com/Consti10 on 09.04.24.
// https://github.com/OpenHD/FPVue_RK3566/tree/openhd
//

#ifndef FPVUE_GSTRTPRECEIVER_H
#define FPVUE_GSTRTPRECEIVER_H

#include <stdint.h>
#include <gst/gst.h>
#include <thread>
#include <memory>
#include <vector>
#include <functional>

#define MAX_PACKET_SIZE 4096
#define RTP_HEADER_LEN 12

enum class VideoCodec {
    UNKNOWN=0,
    H264,
    H265
};

static VideoCodec video_codec(const char * str) {
    if (!strcmp(str, "h264")) {
        return VideoCodec::H264;
    }
    if (!strcmp(str, "h265")) {
        return VideoCodec::H265;
    }
    return VideoCodec::UNKNOWN;
}

/**
 * @brief Uses gstreamer and appsink to expose the functionality of receiving and parsing
 * rtp h264 and h265.
 */
class GstRtpReceiver {
public:
    /**
     * The constructor is delayed, remember to use start_receiving()
     */
    explicit GstRtpReceiver(int udp_port, const VideoCodec& codec);
    explicit GstRtpReceiver(const char *s, const VideoCodec& codec);
    virtual ~GstRtpReceiver();
    // Depending on the codec, these are h264,h265 or mjpeg "frames" / frame buffers
    // The big advantage of gstreamer is that it seems to handle all those parsing quirks the best,
    // e.g. the frames on this cb should be easily passable to whatever decode api is available.
    typedef std::function<void(std::shared_ptr<std::vector<uint8_t>> frame)> NEW_FRAME_CALLBACK;
    void start_receiving(NEW_FRAME_CALLBACK cb);
    void stop_receiving();
    void switch_to_file_playback(const char* file_path);
    void switch_to_stream();
    void fast_forward(double rate = 2.0);
    void fast_rewind(double rate = 2.0);
    void skip_duration(int64_t skip_ms);
    void normal_playback();
    void pause();
    void resume();
private:
    std::string construct_gstreamer_pipeline();
    std::string construct_file_playback_pipeline(const char * file_path);
    void loop_pull_samples();
    void on_new_sample(std::shared_ptr<std::vector<uint8_t>> sample);
    // The gstreamer pipeline
    GstElement * m_gst_pipeline=nullptr;
    NEW_FRAME_CALLBACK m_cb;
    VideoCodec m_video_codec;
    int m_port;
    // appsink
    GstElement *m_app_sink_element = nullptr;
    bool m_pull_samples_run;
    std::unique_ptr<std::thread> m_pull_samples_thread=nullptr;
    // appsrc
    const char* unix_socket = nullptr;
    int sock = -1;
    bool m_read_socket_run = false;
    std::unique_ptr<std::thread> m_read_socket_thread;

    // dvr
    void set_playback_rate(double rate);
    double m_playback_rate = 1.0;
    bool m_is_paused = false;
    double m_pre_pause_rate = 1.0;
};

void idr_set_enabled(bool enabled);
void idr_request_record_start();
void idr_request_decoder_issue(const char* reason);
void idr_notify_decoded_frame();

#endif //FPVUE_GSTRTPRECEIVER_H
