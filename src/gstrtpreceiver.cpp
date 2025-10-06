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
#include <yaml-cpp/yaml.h>
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
#include <pthread.h>
#include <mutex>
std::mutex m_switch_mutex;
extern YAML::Node config;

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
    splash_enabled = config["splash"]["enabled"].as<bool>(false);
    splash_file    = config["splash"]["file"].as<std::string>("");
    splash_timeout = config["splash"]["timeout"].as<int>(3);
}

GstRtpReceiver::GstRtpReceiver(const char *s, const VideoCodec& codec) {
    unix_socket = strdup(s);
    m_video_codec = codec;
    initGstreamerOrThrow();
    splash_enabled = config["splash"]["enabled"].as<bool>(false);
    splash_file    = config["splash"]["file"].as<std::string>("");
    splash_timeout = config["splash"]["timeout"].as<int>(3);

    spdlog::debug("Creating receiver socket on {}", unix_socket);

    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        throw std::runtime_error(std::string("socket() failed: ") + strerror(errno));
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;

    // Abstract socket: Start sun_path with a null byte, then copy the rest.
    // The "@" in logs is a placeholder for the null byte.
    addr.sun_path[0] = '\0';  // First byte is null
    strncpy(addr.sun_path + 1, unix_socket, sizeof(addr.sun_path) - 2);  // Leave room for null
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';  // Ensure null-terminated

    // Length = sizeof(sun_family) + 1 (null byte) + strlen(path)
    socklen_t addr_len = sizeof(addr.sun_family) + 1 + strlen(unix_socket);

    if (bind(sock, (struct sockaddr*)&addr, addr_len) < 0) {
        close(sock);
        throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));
    }

    spdlog::debug("Bound successfully to abstract socket: @{}", unix_socket);
}

GstRtpReceiver::~GstRtpReceiver(){
    close(sock);
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

    // Branch 1: Primary UDP/appsrc stream
    if (!unix_socket) {
        ss << "udpsrc name=udpsrc port=" << m_port << " "
           << pipeline::gst_create_rtp_caps(m_video_codec) << " ! "
           << pipeline::create_rtp_depacketize_for_codec(m_video_codec)
           << pipeline::create_parse_for_codec(m_video_codec)
           << "selector.sink_0 ";
    } else {
        ss << "appsrc name=appsrc " << pipeline::gst_create_rtp_caps(m_video_codec) << " ! "
           << pipeline::create_rtp_depacketize_for_codec(m_video_codec)
           << pipeline::create_parse_for_codec(m_video_codec)
           << "selector.sink_0 ";
    }

    // Banch 2: Backup filesrc
    if (splash_enabled) {
    ss << "filesrc name=filesrc location=\"" <<  splash_file  << "\" ! qtdemux ! "
        << "queue max-size-buffers=0 max-size-time=0 max-size-bytes=0 ! "
        << pipeline::create_parse_for_codec(m_video_codec)
        << "selector.sink_1 ";
    }

    // Final path: selector to appsink
    // CRITICAL: Use sync-streams=true to prevent timestamp issues
    ss << "input-selector name=selector ! "
       << pipeline::create_out_caps(m_video_codec)
       << "appsink drop=true max-buffers=3 name=out_appsink";
       
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
static constexpr int SOCKET_POLL_TIMEOUT_MS = 100;

static void loop_read_socket(bool& keep_looping, int sock_fd, GstAppSrc* appsrc) {
    GstBufferPool* pool = GST_BUFFER_POOL(g_object_get_data(G_OBJECT(appsrc), "buffer-pool"));
    uint64_t pkt_counter = 0;
    auto last_report = std::chrono::steady_clock::now();

    while (keep_looping) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock_fd, &read_fds);

        struct timeval timeout = { .tv_sec = 0, .tv_usec = SOCKET_POLL_TIMEOUT_MS * 1000 };
        int ready = select(sock_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        // Get buffer from pool
        GstBuffer* buffer = nullptr;
        GstFlowReturn ret = gst_buffer_pool_acquire_buffer(pool, &buffer, nullptr);
        if (ret != GST_FLOW_OK || !buffer) {
            spdlog::warn("Failed to acquire buffer from pool");
            continue;
        }

        // Map buffer for writing
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            spdlog::warn("Failed to map buffer");
            gst_buffer_unref(buffer);
            continue;
        }

        // Read data directly into buffer
        ssize_t n = recv(sock_fd, map.data, map.size, 0);
        gst_buffer_unmap(buffer, &map);
        
        if (n <= RTP_HEADER_LEN) {
            spdlog::warn("Invalid RTP packet size: {}", n);
            gst_buffer_unref(buffer);
            continue;
        }

        // Resize buffer to actual data size
        gst_buffer_resize(buffer, 0, n);

        // Push to appsrc
        ret = gst_app_src_push_buffer(appsrc, buffer);
        if (ret != GST_FLOW_OK) {
            spdlog::warn("Appsrc push error: {}", gst_flow_get_name(ret));
            break;
        }

        // Log packet rate (optional)
        pkt_counter++;
        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            spdlog::debug("socket pkts/s {}", pkt_counter);
            pkt_counter = 0;
            last_report = now;
        }
    }
    
    if (pool) {
        gst_buffer_pool_set_active(pool, FALSE);
        gst_object_unref(pool);
    }
}

void GstRtpReceiver::start_receiving(NEW_FRAME_CALLBACK cb) {
    spdlog::info("GstRtpReceiver::start_receiving begin");
    assert(m_gst_pipeline == nullptr);
    m_cb = cb;

    switch_to_stream();

    spdlog::info("GstRtpReceiver::start_receiving end");
}

void GstRtpReceiver::stop_receiving() {
    spdlog::info("GstRtpReceiver::stop_receiving start");
    
    // Stop threads first
    m_pull_samples_run = false;
    m_read_socket_run = false;
    m_drain_watcher_run = false;
    
    if (m_drain_watcher) {
        m_drain_watcher->join();
        m_drain_watcher = nullptr;
    }
    
    if (m_pull_samples_thread) {
        m_pull_samples_thread->join();
        m_pull_samples_thread = nullptr;
    }
    
    if (m_read_socket_thread) {
        m_read_socket_thread->join();
        m_read_socket_thread = nullptr;
    }
    
    // Remove probe if it exists
    if (buffer_probe_id != 0 && m_gst_pipeline) {
        GstElement* src;
        if (!unix_socket) {
            src = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "udpsrc");
        } else {
            src = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "appsrc");
        }
        if (src) {
            GstPad *src_pad = gst_element_get_static_pad(src, "src");
            if (src_pad) {
                gst_pad_remove_probe(src_pad, buffer_probe_id);
                gst_object_unref(src_pad);
            }
            gst_object_unref(src);
        }
        buffer_probe_id = 0;
    }
    
    // Clean up pipeline
    if (m_gst_pipeline != nullptr) {
        gst_element_send_event((GstElement*)m_gst_pipeline, gst_event_new_eos());
        gst_element_set_state(m_gst_pipeline, GST_STATE_NULL);
        gst_object_unref(m_gst_pipeline);
        m_gst_pipeline = nullptr;
    }

    if (m_eos_probe_id != 0 && m_input_selector) {
        GstPad* filesrc_pad = gst_element_get_static_pad(m_input_selector, "sink_1");
        if (filesrc_pad) {
            gst_pad_remove_probe(filesrc_pad, m_eos_probe_id);
            gst_object_unref(filesrc_pad);
            spdlog::debug("Removed EOS probe.");
        }
        m_eos_probe_id = 0;
    }

    if (m_bus) {
        gst_bus_remove_watch(m_bus);
        gst_object_unref(m_bus);
        m_bus = nullptr;
    }

    // Remove pad blocks
    if (m_fallback_src_pad && m_fallback_block_id != 0) {
        gst_pad_remove_probe(m_fallback_src_pad, m_fallback_block_id);
        m_fallback_block_id = 0;
    }
    
    // Unref pads
    if (m_fallback_src_pad) {
        gst_object_unref(m_fallback_src_pad);
        m_fallback_src_pad = nullptr;
    }
    
    m_input_selector = nullptr;
    m_app_sink_element = nullptr;
    
    spdlog::info("GstRtpReceiver::stop_receiving end");
}

std::string GstRtpReceiver::construct_file_playback_pipeline(const char * file_path) {
    std::stringstream ss;
    ss<<"filesrc location="<<file_path<<" ! qtdemux ! ";
    ss<<pipeline::create_parse_for_codec(m_video_codec);
    ss << pipeline::create_out_caps(m_video_codec);
    ss << " appsink drop=true name=out_appsink";
    return ss.str();
}

void GstRtpReceiver::switch_to_file_playback(const char * file_path) {
    stop_receiving();
    
    const auto pipeline = construct_file_playback_pipeline(file_path);
    GError* error = nullptr;
    m_gst_pipeline = gst_parse_launch(pipeline.c_str(), &error);
    spdlog::info("GSTREAMER FILE PLAYBACK PIPE=[{}]", pipeline);
    
    if (error) {
        spdlog::error("gst_parse_launch error: {}", error->message);
        g_error_free(error);
        return;
    }
    
    if (!m_gst_pipeline || !(GST_IS_PIPELINE(m_gst_pipeline))) {
        spdlog::error("Cannot construct file playback pipeline");
        m_gst_pipeline = nullptr;
        return;
    }

    // Setup appsink
    m_app_sink_element = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "out_appsink");
    assert(m_app_sink_element);
    
    gst_element_set_state(m_gst_pipeline, GST_STATE_PLAYING);
    
    m_pull_samples_run = true;
    m_pull_samples_thread = std::make_unique<std::thread>(&GstRtpReceiver::loop_pull_samples, this);
}

static std::atomic<gint64> last_data_time = {0};
static std::atomic<bool> using_primary = {true};

static GstPadProbeReturn
buffer_probe_callback(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    GstRtpReceiver* receiver = static_cast<GstRtpReceiver*>(user_data);
    
    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        // Update timestamp atomically
        last_data_time.store(g_get_monotonic_time());

        // If we are not currently on primary, switch back
        if (!using_primary.load()) {
            receiver->switch_to_primary_source();
        }
    }
    return GST_PAD_PROBE_OK;
}

static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data) {
    GstRtpReceiver* receiver = static_cast<GstRtpReceiver*>(data);

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug_info;
            gst_message_parse_error(msg, &err, &debug_info);
            spdlog::error("GStreamer Error: {}, {}", err->message, debug_info);
            g_clear_error(&err);
            g_free(debug_info);
            break;
        }
        case GST_MESSAGE_EOS:
            // This will likely never be called in this pipeline, but is good practice
            spdlog::info("Main pipeline reached EOS.");
            break;
        
        // Handle our custom message
        case GST_MESSAGE_APPLICATION: {
            const GstStructure *s = gst_message_get_structure(msg);
            if (gst_structure_has_name(s, "loop-file-source")) {
                spdlog::info("Bus received 'loop-file-source', seeking...");
                // It is now safe to perform the seek from this thread.
                gst_element_seek_simple(receiver->get_pipeline(), GST_FORMAT_TIME,
                                        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
            }
            break;
        }

        default:
            break;
    }
    return TRUE; // Keep the watch active
}

static GstPadProbeReturn
filesrc_eos_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    if (!(info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM)) {
        return GST_PAD_PROBE_OK;
    }

    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_EOS) {
        GstRtpReceiver* receiver = static_cast<GstRtpReceiver*>(user_data);
        spdlog::info("Caught EOS on filesrc branch, posting 'loop-file' message...");

        // Post a custom application message to the bus instead of seeking directly.
        // This is thread-safe.
        gst_element_post_message(receiver->get_pipeline(),
            gst_message_new_application(
                GST_OBJECT(receiver->get_pipeline()),
                gst_structure_new_empty("loop-file-source")
            )
        );

        // Drop the EOS event to prevent it from deactivating the selector pad.
        return GST_PAD_PROBE_DROP;
    }

    return GST_PAD_PROBE_OK;
}


void GstRtpReceiver::poll_bus() {
    if (!m_bus) return;
    
    GstMessage *msg = gst_bus_pop(m_bus);
    while (msg) {
        bus_callback(m_bus, msg, this);
        gst_message_unref(msg);
        msg = gst_bus_pop(m_bus);
    }
}

void GstRtpReceiver::switch_to_primary_source() {
    std::lock_guard<std::mutex> lock(m_switch_mutex);
    
    if (using_primary.load()) {
        return; // Already on primary
    }

    if (!splash_enabled) // switching is disabled, maybe do other useful stuff above
        return; 
    
    spdlog::info("Data Resumed! Switching back to default source.");
    
    if (m_input_selector) {


        if (m_fallback_src_pad && m_fallback_block_id == 0) {
            m_fallback_block_id = gst_pad_add_probe(m_fallback_src_pad,
                GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                NULL, NULL, NULL);
            spdlog::debug("Blocked fallback source");
        }

        GstPad* pad = gst_element_get_static_pad(m_input_selector, "sink_0");
        if (pad) {
            g_object_set(m_input_selector, "active-pad", pad, NULL);
            gst_object_unref(pad);
            
            // Reset timestamp to prevent immediate timeout
            last_data_time.store(g_get_monotonic_time());
            using_primary.store(true);
        }
    }
}

void GstRtpReceiver::switch_to_backup_source() {
    std::lock_guard<std::mutex> lock(m_switch_mutex);
    
    if (!using_primary.load()) {
        return; // Already on backup
    }

    if (! splash_enabled) // switching is disabled, maybe do other useful stuff above
        return;

    spdlog::warn("Data Timeout! Switching to backup source.");
    
    if (m_input_selector) {
        GstPad* pad = gst_element_get_static_pad(m_input_selector, "sink_1");
        if (pad) {
            g_object_set(m_input_selector, "active-pad", pad, NULL);
            gst_object_unref(pad);
            using_primary.store(false);
        }

        if (m_fallback_src_pad && m_fallback_block_id != 0) {
            gst_pad_remove_probe(m_fallback_src_pad, m_fallback_block_id);
            m_fallback_block_id = 0;
            spdlog::debug("Unblocked fallback source");
        }

    }
}

void GstRtpReceiver::drain_watcher()
{
    const gint64 TIMEOUT_MICROSECONDS = splash_timeout * G_TIME_SPAN_SECOND; // 3 seconds
    const int CHECK_INTERVAL_MS = 500; // Check every 500ms
    
    while (m_drain_watcher_run) {
        // Only check for timeout if we are currently on primary
        if (using_primary.load()) {
            gint64 current_time = g_get_monotonic_time();
            gint64 last_time = last_data_time.load();
            gint64 time_since_data = current_time - last_time;

            // Only switch if we have a valid timestamp and exceeded timeout
            if (last_time > 0 && time_since_data > TIMEOUT_MICROSECONDS) {
                switch_to_backup_source();
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_INTERVAL_MS));
    }
}

void GstRtpReceiver::switch_to_stream() {
    stop_receiving();
    
    // Reset atomic state
    using_primary.store(true);
    last_data_time.store(0);
    
    const auto pipeline = construct_gstreamer_pipeline();
    GError* error = nullptr;
    m_gst_pipeline = gst_parse_launch(pipeline.c_str(), &error);
    spdlog::info("GSTREAMER STREAM PIPE=[{}]", pipeline);
    
    if (error) {
        spdlog::error("gst_parse_launch error: {}", error->message);
        g_error_free(error);
        return;
    }
    
    if (!m_gst_pipeline || !(GST_IS_PIPELINE(m_gst_pipeline))) {
        spdlog::error("Cannot construct streaming pipeline");
        m_gst_pipeline = nullptr;
        return;
    }

    m_bus = gst_element_get_bus(m_gst_pipeline);
    gst_bus_add_watch(m_bus, bus_callback, this);

    // Get input selector
    m_input_selector = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "selector");
    if (!m_input_selector) {
        spdlog::error("Could not find element 'selector' in the pipeline.");
        gst_object_unref(m_gst_pipeline);
        m_gst_pipeline = nullptr;
        return;
    }

    if (splash_enabled) {
        GstPad* filesrc_pad = gst_element_get_static_pad(m_input_selector, "sink_1");
        if (filesrc_pad) {
            m_eos_probe_id = gst_pad_add_probe(filesrc_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                                            filesrc_eos_probe_cb, this, NULL);
            gst_object_unref(filesrc_pad);
            spdlog::info("Added EOS probe to loop filesrc.");
        } else {
            spdlog::warn("Could not find selector:sink_1 to add EOS probe.");
        }

        GstElement* filesrc = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "filesrc");
        m_fallback_src_pad = gst_element_get_static_pad(filesrc, "src");
        if (m_fallback_src_pad) {
            m_fallback_block_id = gst_pad_add_probe(m_fallback_src_pad,
                GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                NULL, NULL, NULL);
            spdlog::info("Initially blocked fallback source");
        }
        gst_object_unref(filesrc);
    }

    // Set initial active pad to primary (sink_0)
    GstPad* initial_pad = gst_element_get_static_pad(m_input_selector, "sink_0");
    if (initial_pad) {
        g_object_set(m_input_selector, "active-pad", initial_pad, NULL);
        gst_object_unref(initial_pad);
        spdlog::info("Set initial active pad to sink_0 (Primary stream)");
    }

    // If using Unix socket, setup appsrc with buffer pool
    if (unix_socket) {
        GstElement* appsrc = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "appsrc");
        if (!appsrc) {
            spdlog::error("Failed to get appsrc element from pipeline");
            return;
        }
        
        // Configure appsrc with buffer pool
        GstBufferPool* pool = nullptr;
        GstStructure* config = nullptr;
        
        g_object_set(appsrc,
            "stream-type", 0,
            "is-live", TRUE,
            "format", GST_FORMAT_TIME,
            "block", FALSE,
            "do-timestamp", TRUE,
            NULL);
            
        // Create buffer pool
        pool = gst_buffer_pool_new();
        config = gst_buffer_pool_get_config(pool);
        
        GstCaps* caps = gst_caps_new_simple("application/x-rtp",
            "media", G_TYPE_STRING, "video",
            "encoding-name", G_TYPE_STRING, 
                (m_video_codec == VideoCodec::H264) ? "H264" : "H265",
            NULL);
        
        gst_buffer_pool_config_set_params(config, caps, MAX_PACKET_SIZE, 10, 20);
        gst_buffer_pool_set_config(pool, config);
        gst_caps_unref(caps);
        
        if (!gst_buffer_pool_set_active(pool, TRUE)) {
            spdlog::error("Failed to activate buffer pool");
            gst_object_unref(pool);
        } else {
            g_object_set_data(G_OBJECT(appsrc), "buffer-pool", pool);
        }
            
        // Start socket reading thread
        m_read_socket_run = true;
        m_read_socket_thread = std::make_unique<std::thread>([this, appsrc]() {
            pthread_setname_np(pthread_self(), "socket-reader");
            loop_read_socket(m_read_socket_run, this->sock, GST_APP_SRC(appsrc));
        });
    }

    // Setup appsink
    m_app_sink_element = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "out_appsink");
    if (!m_app_sink_element) {
        spdlog::error("Failed to get appsink element");
        return;
    }
    
    // Add buffer probe to monitor traffic
    GstElement* src;
    if (!unix_socket) {
        src = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "udpsrc");
    } else {
        src =gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "appsrc");
    }
    if (src) {
        GstPad *src_pad = gst_element_get_static_pad(src, "src");
        if (src_pad) {
            // Pass 'this' pointer instead of selector
            buffer_probe_id = gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                                               buffer_probe_callback, this, NULL);
            gst_object_unref(src_pad);
        }
        gst_object_unref(src);
    }

    // Initialize timestamp BEFORE starting pipeline
    last_data_time.store(g_get_monotonic_time());

    // Start pipeline
    GstStateChangeReturn ret = gst_element_set_state(m_gst_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("Failed to set pipeline to PLAYING state");
        return;
    }

    // Start threads
    m_pull_samples_run = true;
    m_pull_samples_thread = std::make_unique<std::thread>(&GstRtpReceiver::loop_pull_samples, this);
    
    m_drain_watcher_run = true;
    m_drain_watcher = std::make_unique<std::thread>(&GstRtpReceiver::drain_watcher, this);
}

void GstRtpReceiver::set_playback_rate(double rate) {
    if (!m_gst_pipeline) {
        spdlog::warn("Cannot set playback rate: pipeline is not running.");
        return;
    }
    

    spdlog::info("Setting playback rate to: {}", rate);

    // To change the playback rate, we seek to the current position with a new rate.
    // The flags ensure that the pipeline flushes old data and continues smoothly.
    GstEvent *seek_event = gst_event_new_seek(
        rate,
        GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_NONE, 0, // start from current position
        GST_SEEK_TYPE_NONE, 0  // do not change stop position
    );

    if (!gst_element_send_event(m_gst_pipeline, seek_event)) {
        spdlog::warn("Failed to send seek event to change playback rate.");
    } else {
        m_playback_rate = rate;
    }
}

void GstRtpReceiver::fast_forward(double rate) {
    if (rate <= 1.0) {
        spdlog::warn("Fast forward rate must be greater than 1.0. Using 2.0 instead.");
        rate = 2.0;
    }
    set_playback_rate(rate);
}

void GstRtpReceiver::fast_rewind(double rate) {
    if (rate <= 1.0) {
        spdlog::warn("Fast rewind rate must be greater than 1.0. Using 2.0 instead.");
        rate = 2.0;
    }
    // For rewind, the rate must be negative
    set_playback_rate(-rate);
}

void GstRtpReceiver::normal_playback() {
    set_playback_rate(1.0);
}

void GstRtpReceiver::pause() {
    if (!m_gst_pipeline) {
        spdlog::warn("Cannot pause: pipeline is not running.");
        return;
    }

    // If we're already paused, do nothing
    if (m_is_paused) {
        spdlog::debug("Pipeline is already paused.");
        return;
    }

    // Store current playback rate before pausing
    m_pre_pause_rate = m_playback_rate;
    
    // Set pipeline to PAUSED state
    GstStateChangeReturn ret = gst_element_set_state(m_gst_pipeline, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("Failed to pause pipeline");
        return;
    }

    // Wait for state change to complete
    ret = gst_element_get_state(m_gst_pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("Failed to complete pause operation");
        return;
    }

    m_is_paused = true;
    spdlog::info("Pipeline paused");
}

void GstRtpReceiver::resume() {
    if (!m_gst_pipeline) {
        spdlog::warn("Cannot resume: pipeline is not running.");
        return;
    }

    // If we're not paused, do nothing
    if (!m_is_paused) {
        spdlog::debug("Pipeline is not paused.");
        return;
    }

    // Set pipeline back to PLAYING state
    GstStateChangeReturn ret = gst_element_set_state(m_gst_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("Failed to resume pipeline");
        return;
    }

    // Restore previous playback rate if it wasn't normal
    if (m_pre_pause_rate != 1.0) {
        set_playback_rate(m_pre_pause_rate);
    }

    m_is_paused = false;
    spdlog::info("Pipeline resumed");
}

void GstRtpReceiver::skip_duration(int64_t skip_ms) {
    if (!m_gst_pipeline) {
        spdlog::warn("Cannot skip: pipeline is not running.");
        return;
    }

    if (skip_ms == 0) {
        spdlog::debug("Skip duration is zero - no action taken.");
        return;
    }

    // Get current position
    gint64 current_pos;
    if (!gst_element_query_position(m_gst_pipeline, GST_FORMAT_TIME, &current_pos)) {
        spdlog::warn("Could not query current position");
        return;
    }

    // Calculate new position (convert skip_ms to nanoseconds)
    gint64 new_pos = current_pos + (skip_ms * GST_MSECOND);
    
    // Clamp the position to valid range
    if (new_pos < 0) {
        new_pos = 0;
        spdlog::debug("Clamped skip to start of stream");
    }

    spdlog::info("Skipping {} ms (from {} to {} ms)",
                skip_ms,
                current_pos / GST_MSECOND,
                new_pos / GST_MSECOND);

    // Create seek event
    GstEvent* seek_event = gst_event_new_seek(
        1.0,  // Normal playback rate
        GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET, new_pos,  // start from new position
        GST_SEEK_TYPE_NONE, 0        // do not change stop position
    );

    if (!gst_element_send_event(m_gst_pipeline, seek_event)) {
        spdlog::warn("Failed to send seek event for skipping.");
    }
}