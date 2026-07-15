//
// Created by https://github.com/Consti10 on 09.04.24.
// https://github.com/OpenHD/FPVue_RK3566/tree/openhd
//

#ifndef FPVUE_GSTRTPRECEIVER_H
#define FPVUE_GSTRTPRECEIVER_H

#include <stdint.h>
#ifndef USE_SIMULATOR
#include <gst/gst.h>
#endif
#include <stdbool.h>
#ifdef __cplusplus
#include <thread>
#include <memory>
#include <vector>
#include <functional>
#include <mutex>
#include <string>

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
    // Enable receiving Opus audio muxed into the same RTP flow as the video
    // (distinguished by payload type). Must be called before start_receiving().
    // device is an ALSA device string ("" = system default); pt is the audio
    // RTP payload type (OpenIPC/majestic default 98).
    void configure_audio(bool enabled, const std::string& device, int pt);
    // Toggle the Opus audio branch at runtime (e.g. from the OSD menu). Rebuilds
    // the live streaming pipeline; a no-op if the state is unchanged or while a
    // DVR file is playing (the choice then applies on the next switch_to_stream).
    void set_audio_enabled(bool enabled);
    // Reports the *effective* state (is the Opus branch actually up), not just the
    // intent — so the OSD switch reads off when the selected sink is unavailable
    // and audio has fallen back to video-only.
    bool get_audio_enabled() const { return m_audio_active; }
    // Select the ALSA output for audio: an /proc/asound/cards id (e.g.
    // "rockchiphdmi"), a full ALSA device string, or "" / "default" for the
    // system default. Rebuilds the live pipeline if audio is currently playing.
    void set_audio_device(const std::string& device);
    // Depending on the codec, these are h264,h265 or mjpeg "frames" / frame buffers
    // The big advantage of gstreamer is that it seems to handle all those parsing quirks the best,
    // e.g. the frames on this cb should be easily passable to whatever decode api is available.
    typedef std::function<void(std::shared_ptr<std::vector<uint8_t>> frame)> NEW_FRAME_CALLBACK;
    void start_receiving(NEW_FRAME_CALLBACK cb);
    void stop_receiving();
    VideoCodec switch_to_file_playback(const char* file_path);
    void switch_to_stream();
    void fast_forward(double rate = 2.0);
    void fast_rewind(double rate = 2.0);
    void skip_duration(int64_t skip_ms);
    void normal_playback();
    void pause();
    void resume();
    // The codec the live pipeline is currently built for. When constructed with
    // VideoCodec::UNKNOWN ("auto") this defaults to H.265 in switch_to_stream()
    // and is updated if mid-stream detection flips it; valid only after
    // start_receiving()/switch_to_stream().
    VideoCodec get_active_codec() const { return m_video_codec; }
    // Invoked (on an internal thread) after a mid-stream codec switch has been
    // detected and the pipeline rebuilt, so the host can realign its decoder.
    void set_codec_changed_callback(std::function<void(VideoCodec)> cb);
private:
    // Rebuild the pipeline for new_codec after a mid-stream switch is detected.
    void request_codec_switch(VideoCodec new_codec);
    std::string construct_gstreamer_pipeline();
    std::string construct_file_playback_pipeline(const char * file_path);
    void loop_pull_samples();
    void on_new_sample(std::shared_ptr<std::vector<uint8_t>> sample);
    // The gstreamer pipeline
    GstElement * m_gst_pipeline=nullptr;
    NEW_FRAME_CALLBACK m_cb;
    VideoCodec m_video_codec;
    // True when constructed with VideoCodec::UNKNOWN ("auto"): the pipeline
    // builds for H.265 and mid-stream codec-switch detection is enabled. False
    // when the user pinned a codec, in which case we never override their choice.
    bool m_auto_codec = false;
    // Audio (Opus) config. m_audio_enabled / m_audio_device are the user's intent
    // (what the CLI/menu asked for, and what get_audio_enabled() reports).
    // m_audio_active is the effective state after switch_to_stream() checks the
    // Opus/ALSA stack and the selected output device are usable; when the sink is
    // missing it drops to false (video-only) while the intent/selection is kept.
    bool m_audio_enabled = false;
    bool m_audio_active = false;
    std::string m_audio_device;
    int m_audio_pt = 98;
    // True while a switch_to_file_playback() pipeline is up (no live audio branch).
    bool m_file_playback = false;
    // Serializes switch_to_stream() so a menu audio-toggle and an automatic
    // codec-switch rebuild can never tear down/rebuild the pipeline at once.
    std::mutex m_stream_mutex;
    VideoCodec m_playback_codec = VideoCodec::UNKNOWN;
    // Notified after a detected mid-stream codec switch + pipeline rebuild.
    std::function<void(VideoCodec)> m_on_codec_changed;
    std::mutex m_codec_changed_mutex;
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
#endif


#ifdef __cplusplus
extern "C" {
#endif
void idr_set_enabled(bool enabled);
bool idr_get_enabled();
void restream_set_enabled(bool enabled);
bool restream_get_enabled();
void restream_scan_clients(char* buf, size_t buf_len);
void restream_set_manual_ip(const char* ip);
const char* restream_get_manual_ip();
void restream_set_pinned_ip(const char* ip);
void idr_request_record_start();
void idr_request_decoder_issue(const char* reason);
void idr_notify_decoded_frame();
#ifdef __cplusplus
}
#endif

#endif //FPVUE_GSTRTPRECEIVER_H
