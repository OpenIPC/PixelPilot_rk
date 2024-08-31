//
// Created by https://github.com/Consti10 on 09.04.24.
// https://github.com/OpenHD/FPVue_RK3566/tree/openhd
//

#ifndef FPVUE_GSTRTPRECEIVER_H
#define FPVUE_GSTRTPRECEIVER_H

#include <gst/gst.h>
#include <thread>
#include <memory>
#include <vector>
#include <functional>

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
    virtual ~GstRtpReceiver();
    // Depending on the codec, these are h264,h265 or mjpeg "frames" / frame buffers
    // The big advantage of gstreamer is that it seems to handle all those parsing quirks the best,
    // e.g. the frames on this cb should be easily passable to whatever decode api is available.
    typedef std::function<void(std::shared_ptr<std::vector<uint8_t>> frame)> NEW_FRAME_CALLBACK;
    void start_receiving(NEW_FRAME_CALLBACK cb);
    void stop_receiving();
private:
    std::string construct_gstreamer_pipeline();
    void loop_pull_samples();
    void on_new_sample(std::shared_ptr<std::vector<uint8_t>> sample);
private:
    // The gstreamer pipeline
    GstElement * m_gst_pipeline=nullptr;
    NEW_FRAME_CALLBACK m_cb;
    VideoCodec m_video_codec;
    int m_port;
    // appsink
    GstElement *m_app_sink_element = nullptr;
    bool m_pull_samples_run;
    std::unique_ptr<std::thread> m_pull_samples_thread=nullptr;
};


#endif //FPVUE_GSTRTPRECEIVER_H
