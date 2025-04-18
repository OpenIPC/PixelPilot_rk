//
// Created by https://github.com/Consti10 on 09.04.24.
// https://github.com/OpenHD/FPVue_RK3566/tree/openhd
//

#include "gstrtpreceiver.h"
#include "gst/gstparse.h"
#include "gst/gstpipeline.h"
#include "gst/app/gstappsink.h"
#include "gst/app/gstappsrc.h"
#include "spdlog/spdlog.h"
#include <cstring>
#include <stdexcept>
#include <cassert>
#include <sstream>
#include <iostream>
#include <memory>
#include <utility>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/un.h>
#include <fcntl.h>

namespace pipeline {
    static std::string gst_create_rtp_caps(const VideoCodec& videoCodec){
        std::stringstream ss;
        if(videoCodec==VideoCodec::H264){
            ss<<"caps=\"application/x-rtp, media=(string)video, encoding-name=(string)H264, payload=(int)96\"";
        }else if(videoCodec==VideoCodec::H265){
            ss<<"caps=\"application/x-rtp, media=(string)video, encoding-name=(string)H265, clock-rate=(int)90000\"";
        }
        return ss.str();
    }
    static std::string create_rtp_depacketize_for_codec(const VideoCodec& codec){
        if(codec==VideoCodec::H264)return "rtph264depay ! ";
        if(codec==VideoCodec::H265)return "rtph265depay ! ";
        assert(false);
        return "";
    }
    static std::string create_parse_for_codec(const VideoCodec& codec){
        // config-interval=-1 = makes 100% sure each keyframe has SPS and PPS
        if(codec==VideoCodec::H264)return "h264parse config-interval=-1 ! ";
        if(codec==VideoCodec::H265)return "h265parse config-interval=-1  ! ";
        assert(false);
        return "";
    }
    static std::string create_out_caps(const VideoCodec& codec){
        if(codec==VideoCodec::H264){
            std::stringstream ss;
            ss<<"video/x-h264";
            ss<<", stream-format=\"byte-stream\",alignment=nal";
            //ss<<", alignment=\"nal\"";
            ss<<" ! ";
            return ss.str();
        }else if(codec==VideoCodec::H265){
            std::stringstream ss;
            ss<<"video/x-h265";
            ss<<", stream-format=\"byte-stream\"";
            //ss<<", alignment=\"nal\"";
            ss<<" ! ";
            return ss.str();
        }
        assert(false);
    }
}

static void initGstreamerOrThrow() {
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        g_error_free(error);
        throw std::runtime_error("GStreamer initialization failed");
    }
}

GstRtpReceiver::GstRtpReceiver(int udp_port, const VideoCodec& codec)
{
    m_port=udp_port;
    m_video_codec=codec;
    initGstreamerOrThrow();

    spdlog::debug("Creating receiver socket");
    unlink(SOCKET_PATH);
    sock=socket(AF_UNIX,SOCK_DGRAM,0);
    if(sock<0){
        perror("socket");
    }
    struct sockaddr_un addr={0};
    addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path,SOCKET_PATH,sizeof(addr.sun_path) - 1);
    if (bind(sock,(struct sockaddr*)&addr,sizeof(addr))<0)
    {
        perror("bind");
    }

}


GstRtpReceiver::~GstRtpReceiver(){
    close(sock);
    unlink(SOCKET_PATH);
}

static std::shared_ptr<std::vector<uint8_t>> gst_copy_buffer(GstBuffer* buffer){
    assert(buffer);
    const auto buff_size = gst_buffer_get_size(buffer);
    auto ret = std::make_shared<std::vector<uint8_t>>(buff_size);
    GstMapInfo map;
    gst_buffer_map(buffer, &map, GST_MAP_READ);
    assert(map.size == buff_size);
    std::memcpy(ret->data(), map.data, buff_size);
    gst_buffer_unmap(buffer, &map);
    return ret;
}

static void loop_pull_appsink_samples(bool& keep_looping,GstElement *app_sink_element,
                                      const GstRtpReceiver::NEW_FRAME_CALLBACK out_cb){
    assert(app_sink_element);
    assert(out_cb);
    const uint64_t timeout_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(100)).count();
    while (keep_looping){
        //GstSample* sample = nullptr;
        GstSample* sample= gst_app_sink_try_pull_sample(GST_APP_SINK(app_sink_element),timeout_ns);
        if (sample) {
            //gst_debug_sample(sample);
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            if (buffer) {
                auto buff_copy=gst_copy_buffer(buffer);
                out_cb(buff_copy);
            }
            gst_sample_unref(sample);
        }
    }
}


std::string GstRtpReceiver::construct_gstreamer_pipeline()
{
    std::stringstream ss;
    // ss<<"udpsrc port="<<m_port<<" "<<pipeline::gst_create_rtp_caps(m_video_codec)<<" ! ";
    ss<<"appsrc name=appsrc "<<pipeline::gst_create_rtp_caps(m_video_codec)<<" ! ";
    ss<<pipeline::create_rtp_depacketize_for_codec(m_video_codec);
    ss<<pipeline::create_parse_for_codec(m_video_codec);
    ss<<pipeline::create_out_caps(m_video_codec);
    ss<<" appsink drop=true name=out_appsink";
    return ss.str();
}

void GstRtpReceiver::loop_pull_samples()
{
    assert(m_app_sink_element);
    auto cb=[this](std::shared_ptr<std::vector<uint8_t>> sample){
        this->on_new_sample(sample);
    };
    loop_pull_appsink_samples(m_pull_samples_run,m_app_sink_element,cb);
}

void GstRtpReceiver::on_new_sample(std::shared_ptr<std::vector<uint8_t> > sample)
{
    if(m_cb){
        //debug_sample(sample);
        m_cb(sample);
    }else{
    }
}

/* socket → appsrc */
guint  pkt_counter = 0;
time_t last_report = 0;
static gboolean on_socket(GIOChannel *src, GIOCondition cond, gpointer user)
{
    GstAppSrc *appsrc = GST_APP_SRC(user);
    guint8 buf[MAX_PACKET_SIZE];
    ssize_t n = recv(g_io_channel_unix_get_fd(src), buf, sizeof(buf), 0);
    if (n <= RTP_HEADER_LEN) return TRUE;

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, n, NULL);
    gst_buffer_fill(buffer, 0, buf, n);
    if (gst_app_src_push_buffer(appsrc, buffer) != GST_FLOW_OK) {
        g_printerr("push error\n");
        return FALSE;
    }
    pkt_counter++; time_t now=time(NULL);
    if(now!=last_report){g_print("[INFO] pkts/s %u\n",pkt_counter);pkt_counter=0;last_report=now;}
    return TRUE;
}

void GstRtpReceiver::start_receiving(NEW_FRAME_CALLBACK cb)
{
    spdlog::info("GstRtpReceiver::start_receiving begin");
    assert(m_gst_pipeline==nullptr);
    m_cb=cb;

    const auto pipeline=construct_gstreamer_pipeline();
    GError *error = nullptr;
    m_gst_pipeline = gst_parse_launch(pipeline.c_str(), &error);
    spdlog::info("GSTREAMER PIPE=[{}]", pipeline);
    if (error) {
        spdlog::warn("gst_parse_launch error: {}", error->message);
        return;
    }
    if(!m_gst_pipeline || !(GST_IS_PIPELINE(m_gst_pipeline))){
        spdlog::warn("Cannot construct pipeline");
        m_gst_pipeline = nullptr;
        return;
    }

    // Then get the appsrc element by name
    GstElement *appsrc = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "appsrc");
    if (!appsrc) {
        g_printerr("Failed to get appsrc element from pipeline\n");
        // Handle error
    }
    g_object_set(appsrc,
        "stream-type", 0,
        "is-live", TRUE,
        "format", GST_FORMAT_TIME,
        "block", FALSE,
        "max-bytes", 2000000,  // 2MB buffer
        "min-percent", 20,     // Start pushing when 20% empty
        "do-timestamp", TRUE,  // Auto-timestamp buffers
        NULL);

    // Replace g_io_add_watch with this more robust version
    GIOChannel* chan = g_io_channel_unix_new(sock);
    g_io_channel_set_flags(chan, G_IO_FLAG_NONBLOCK, NULL);
    guint watch_id = g_io_add_watch_full(
        chan, 
        G_PRIORITY_HIGH,  // Higher priority
        static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL),
        on_socket,
        appsrc,
        NULL);

    gst_element_set_state (m_gst_pipeline, GST_STATE_PLAYING);

    m_loop = g_main_loop_new(nullptr, FALSE);
    m_main_loop_thread = std::make_unique<std::thread>([this]() {
        g_main_loop_run(this->m_loop);
    });

    //
    // we pull data out of the gst pipeline as cpu memory buffer(s) using the gstreamer "appsink" element
    m_app_sink_element=gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "out_appsink");
    assert(m_app_sink_element);
    m_pull_samples_run= true;
    m_pull_samples_thread=std::make_unique<std::thread>(&GstRtpReceiver::loop_pull_samples, this);

    spdlog::info("GstRtpReceiver::start_receiving end");
}

void GstRtpReceiver::stop_receiving()
{
    m_pull_samples_run=false;
    if(m_pull_samples_thread){
        m_pull_samples_thread->join();
        m_pull_samples_thread=nullptr;
    }
    //TODO unref appsink reference
    if (m_gst_pipeline != nullptr) {
        // Needed on jetson ?!
        gst_element_send_event ((GstElement*)m_gst_pipeline, gst_event_new_eos ());
        gst_element_set_state(m_gst_pipeline, GST_STATE_PAUSED);
        gst_element_set_state (m_gst_pipeline, GST_STATE_NULL);
        gst_object_unref (m_gst_pipeline);
        m_gst_pipeline=nullptr;
    }
    if (m_loop) {
        g_main_loop_quit(m_loop);
        if (m_main_loop_thread && m_main_loop_thread->joinable()) {
            m_main_loop_thread->join();
        }
        g_main_loop_unref(m_loop);
        m_loop = nullptr;
    }
}
